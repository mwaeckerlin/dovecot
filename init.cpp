/**

dovecot init: minimal, shell-free entrypoint for the dovecot
container.

Follows the same three-stage / statically-linked / execv() pattern as
the sibling mwaeckerlin/rspamd and mwaeckerlin/clamav inits: parse env,
write the runtime configuration into /etc/dovecot/conf.d/, exec the
dovecot daemon in foreground. The runtime image contains no shell, no
perl, no busybox.

Behaviour (port of the former start.sh, contract unchanged):

  1. conf.d/passdb-sql.conf (mode 0600): PostfixAdmin database
     credentials for the SQL passdb; DEFAULT_PASS_SCHEME defaults to
     SHA512-CRYPT (PostfixAdmin's php_crypt).
  2. conf.d/10-auth-cleartext.conf: auth_allow_cleartext from
     DOVECOT_ALLOW_CLEARTEXT (default no — cleartext mechanisms only
     over TLS, a password never travels unencrypted).
  3. conf.d/10-debug.conf (log_debug=category=auth) only when
     DOVECOT_DEBUG_AUTH=yes — a diagnostics override, never a
     production default.
  4. sieve/spam-to-junk.sieve according to SPAM_DELIVERY_MODE
     (reject / mark / folder) and pre-compiled with sievec — the mail
     user executing it at delivery time has no write access to
     /etc/dovecot/sieve. A compile error is a hard startup failure.
  5. conf.d/10-ssl.conf when /etc/letsencrypt/live/$DOMAIN/ holds
     fullchain.pem + privkey.pem.
  6. exec dovecot -F.

Supports --healthcheck: TCP-probes the IMAP listener at 127.0.0.1:143.

*/

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *DOVECOT   = "/usr/sbin/dovecot";
constexpr const char *SIEVEC    = "/usr/bin/sievec";
constexpr const char *CONF_D    = "/etc/dovecot/conf.d";
constexpr const char *SIEVE_DIR = "/etc/dovecot/sieve";

std::string
env_or(const char *name, const std::string &fallback = {}) {
  const char *v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

void
write_file(const fs::path &p, const std::string &content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + p.string());
  out << content;
}

// Capture ONLY stdout — stderr stays on the container log, where a
// sievec compile error is fully visible.
int
run_capture(const std::vector<const char *> &argv, std::string &out) {
  int pipefd[2];
  if (pipe(pipefd) < 0) throw std::runtime_error("pipe");
  pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("fork");
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], 1);
    close(pipefd[1]);
    std::vector<char *> a;
    for (auto *s : argv) a.push_back(const_cast<char *>(s));
    a.push_back(nullptr);
    execv(a[0], a.data());
    _exit(127);
  }
  close(pipefd[1]);
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, n);
  close(pipefd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void
write_passdb() {
  const std::string content =
      "mysql maildb {\n"
      "  host = "     + env_or("DB_HOST")     + "\n"
      "  dbname = "   + env_or("DB_NAME")     + "\n"
      "  user = "     + env_or("DB_USER")     + "\n"
      "  password = " + env_or("DB_PASSWORD") + "\n"
      "}\n"
      "\n"
      "passdb sql {\n"
      "  sql_driver = mysql\n"
      "  query = SELECT password FROM mailbox WHERE username = '%{user}'\n"
      "  default_password_scheme = "
      + env_or("DEFAULT_PASS_SCHEME", "SHA512-CRYPT") + "\n"
      "}\n";
  const fs::path p = fs::path(CONF_D) / "passdb-sql.conf";
  write_file(p, content);
  chmod(p.c_str(), 0600);
}

// Force auth over TLS by default (DOVECOT_ALLOW_CLEARTEXT=no):
// cleartext mechanisms are only offered on an encrypted channel, so a
// password is never sent in the clear. This requires TLS to be
// available (a cert at /etc/letsencrypt/live/$DOMAIN/) — without a
// cert AND without explicitly setting DOVECOT_ALLOW_CLEARTEXT=yes
// there is no usable auth, which is the intended secure default.
void
write_auth_cleartext() {
  const std::string allow = env_or("DOVECOT_ALLOW_CLEARTEXT", "no");
  write_file(fs::path(CONF_D) / "10-auth-cleartext.conf",
             "auth_allow_cleartext = " + allow + "\n");
  std::cerr << "**** auth_allow_cleartext = " << allow << std::endl;
}

// Verbose auth debug logging is a deliberate diagnostics override,
// never a production default: auth debug lines expose per-login
// details (users, mechanisms, remote IPs) to everyone who can read
// the container logs.
void
write_debug() {
  if (env_or("DOVECOT_DEBUG_AUTH", "no") != "yes") return;
  write_file(fs::path(CONF_D) / "10-debug.conf",
             "log_debug = category=auth\n");
  std::cerr << "**** auth debug logging enabled (DOVECOT_DEBUG_AUTH=yes)"
            << std::endl;
}

// SPAM_DELIVERY_MODE — write the sieve_before script that decides how
// mail carrying `X-Spam-Flag: YES` (added by rspamd milter_headers
// above the add-header score, but not above the reject score) is
// delivered. Default is `reject` (nothing above reject score reaches
// here — the file is then a no-op that just documents the mode).
void
write_spam_delivery_mode() {
  const std::string mode = env_or("SPAM_DELIVERY_MODE", "reject");
  std::string script;
  if (mode == "folder") {
    script =
        "require [\"fileinto\", \"mailbox\"];\n"
        "# SPAM_DELIVERY_MODE=folder — deliver spam to the Junk folder.\n"
        "if header :contains \"X-Spam-Flag\" \"YES\" {\n"
        "    fileinto :create \"Junk\";\n"
        "    stop;\n"
        "}\n";
    std::cerr << "**** SPAM_DELIVERY_MODE=folder — spam → Junk" << std::endl;
  } else if (mode == "mark") {
    script =
        "# SPAM_DELIVERY_MODE=mark — rspamd already added X-Spam-Flag /\n"
        "# Level / Status headers upstream (see rspamd/milter_headers.conf).\n"
        "# Nothing to do at delivery time; the user's MUA filters on the\n"
        "# headers.\n";
    std::cerr << "**** SPAM_DELIVERY_MODE=mark — X-Spam-* headers only, "
                 "INBOX delivery" << std::endl;
  } else {
    script =
        "# SPAM_DELIVERY_MODE=reject — rspamd rejects everything above\n"
        "# RSPAMD_REJECT_SCORE at SMTP time; nothing reaches this sieve.\n";
    std::cerr << "**** SPAM_DELIVERY_MODE=reject — SMTP-time rejection only"
              << std::endl;
  }
  const fs::path sieve = fs::path(SIEVE_DIR) / "spam-to-junk.sieve";
  write_file(sieve, script);
  // Pre-compile: the mail user executing the script at delivery time
  // has no write access here, so root compiles the .svbin up front. A
  // compile error is a hard startup failure — never silently deliver
  // without the filter.
  std::string out;
  if (run_capture({SIEVEC, sieve.c_str()}, out) != 0)
    throw std::runtime_error("sievec " + sieve.string() + " failed: " + out);
}

void
write_ssl() {
  const std::string domain = env_or("DOMAIN");
  const std::string live = "/etc/letsencrypt/live/" + domain;
  if (!fs::exists(live + "/fullchain.pem") ||
      !fs::exists(live + "/privkey.pem")) return;
  write_file(fs::path(CONF_D) / "10-ssl.conf",
      "ssl = yes\n"
      "ssl_server_cert_file = " + live + "/fullchain.pem\n"
      "ssl_server_key_file = "  + live + "/privkey.pem\n"
      "ssl_server_prefer_ciphers = server\n");
  std::cerr << "**** TLS configured for " << domain << std::endl;
}

// --------------------------------------------------- healthcheck ----------

int
tcp_probe(const std::string &host, int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return 1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  int rc = connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
  close(s);
  return rc == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[]) try {
  if (argc > 1 && std::string(argv[1]) == "--healthcheck")
    return tcp_probe("127.0.0.1", 143);

  fs::create_directories(CONF_D);
  fs::create_directories(SIEVE_DIR);

  write_passdb();
  write_auth_cleartext();
  write_debug();
  write_spam_delivery_mode();
  write_ssl();

  std::cerr << "**** Starting dovecot" << std::endl;
  const char *exec_argv[] = {"dovecot", "-F", nullptr};
  execv(DOVECOT, const_cast<char *const *>(exec_argv));
  std::perror(DOVECOT);
  return 1;
} catch (const std::exception &e) {
  std::cerr << "EXCEPTION: " << e.what() << std::endl;
  return 1;
} catch (...) {
  std::cerr << "UNKNOWN ERROR" << std::endl;
  return 1;
}
