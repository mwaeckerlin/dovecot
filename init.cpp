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

// Every env value is rendered into a dovecot config file (plain
// key=value lines, or an SQL passdb block). An unvalidated value — a
// newline above all — would inject arbitrary extra directives (config
// injection). Operator input is input: whitelist-validate each value
// class and refuse to start on anything malformed. Pinned by
// tests/config-validation.sh.
[[noreturn]] void
die_invalid(const char *var, const std::string &value) {
  std::cerr << "**** ERROR: invalid " << var << " \"" << value
            << "\" — refusing to start" << std::endl;
  std::exit(1);
}

// Empty stays allowed: every knob is optional — validation constrains
// only what IS set.
void
check_chars(const char *var, const std::string &v, const std::string &extra) {
  for (char c : v)
    if (!std::isalnum(static_cast<unsigned char>(c)) &&
        extra.find(c) == std::string::npos)
      die_invalid(var, v);
}

// A secret must not be echoed back into the container log on error.
void
check_no_crlf_secret(const char *var, const std::string &v) {
  if (v.find_first_of("\r\n") != std::string::npos) {
    std::cerr << "**** ERROR: invalid " << var
              << " (contains a newline) — refusing to start" << std::endl;
    std::exit(1);
  }
}

// dovecot data sizes: digits with an optional single k/M/G/T suffix
// ("500M", "1G", "0" = unlimited).
void
check_size(const char *var, const std::string &v) {
  auto suffix = v.find_first_not_of("0123456789");
  if (v.empty() || suffix == 0) die_invalid(var, v);
  if (suffix == std::string::npos) return;
  if (suffix != v.size() - 1 ||
      std::string("kMGT").find(v[suffix]) == std::string::npos)
    die_invalid(var, v);
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

// E2: sieve_max_script_size is configurable (default 500M, following
// the family rule «artificial limits high, but configurable and
// documented»). An operator on a small box can lower the authenticated
// ManageSieve upload/compile DoS surface. Written to conf.d so it wins
// over the local.conf default (conf.d is included after local.conf).
void
write_sieve_limits() {
  const std::string size = env_or("SIEVE_MAX_SCRIPT_SIZE", "500M");
  write_file(fs::path(CONF_D) / "10-sieve-limits.conf",
             "sieve_max_script_size = " + size + "\n");
  std::cerr << "**** sieve_max_script_size = " << size << std::endl;
}

// The userdb provides uid/gid/home for every mailbox. It lives here (not
// in local.conf) so DOVECOT_QUOTA can switch its backend without a
// second, non-merging userdb block:
//   off  → static (uid/gid/home only; identical to the previous default)
//   on   → sql, returning uid/gid/home AND the per-user quota limit from
//          the PostfixAdmin `mailbox.quota` column (bytes; <=0 =
//          unlimited → quota_storage_size stays NULL, user unaffected).
// The sql form reuses the `mysql maildb` connection from passdb-sql.conf.
void
write_userdb() {
  const std::string home =
      "/var/mail/domains/%{user | domain}/%{user | username}";
  std::string content;
  if (env_or("DOVECOT_QUOTA", "no") == "yes") {
    content =
        "userdb sql {\n"
        "  sql_driver = mysql\n"
        "  query = SELECT 5000 AS uid, 5000 AS gid, "
        "CONCAT('/var/mail/domains/', SUBSTRING_INDEX(username,'@',-1), "
        "'/', SUBSTRING_INDEX(username,'@',1)) AS home, "
        "CASE WHEN quota > 0 THEN CONCAT(quota, 'B') END "
        "AS quota_storage_size "
        "FROM mailbox WHERE username = '%{user}'\n"
        "}\n";
    std::cerr << "**** userdb sql (per-mailbox quota from PostfixAdmin)"
              << std::endl;
  } else {
    content =
        "userdb static {\n"
        "  fields {\n"
        "    uid = 5000\n"
        "    gid = 5000\n"
        "    home = " + home + "\n"
        "  }\n"
        "}\n";
  }
  write_file(fs::path(CONF_D) / "05-userdb.conf", content);
}

// E5: optional per-mailbox quota (Dovecot 2.4 syntax). Off by default
// (DOVECOT_QUOTA=no) — no behaviour change. Enables the count-backend
// quota plugin and enforcement; the per-user limit comes via the sql
// userdb (write_userdb). Over-quota delivery tempfails
// (quota_full_tempfail=yes in local.conf; grace 0 so a full mailbox
// defers cleanly instead of a silent one-shot overage), so the sender
// retries and eventually bounces — never a silent drop.
void
write_quota() {
  if (env_or("DOVECOT_QUOTA", "no") != "yes") return;
  write_file(fs::path(CONF_D) / "90-quota.conf",
      "mail_plugins {\n"
      "  quota = yes\n"
      "}\n"
      "\n"
      "protocol imap {\n"
      "  mail_plugins {\n"
      "    imap_quota = yes\n"
      "  }\n"
      "}\n"
      "\n"
      "quota userquota {\n"
      "  driver = count\n"
      "}\n"
      "\n"
      "quota_enforce = yes\n"
      "quota_storage_grace = 0\n");
  std::cerr << "**** per-mailbox quota enabled (PostfixAdmin mailbox.quota)"
            << std::endl;
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
  // Validate every env value before it is rendered into a config file —
  // also on the --healthcheck path, so a misconfigured container reports
  // unhealthy instead of probing a listener that never came up. DB_*
  // land in the SQL passdb / quota dict blocks; a newline would inject
  // extra dovecot directives (config injection).
  check_chars("DB_HOST", env_or("DB_HOST"), ".-_:,");
  check_chars("DB_NAME", env_or("DB_NAME"), "._-");
  check_chars("DB_USER", env_or("DB_USER"), "._-");
  check_no_crlf_secret("DB_PASSWORD", env_or("DB_PASSWORD"));
  check_chars("DEFAULT_PASS_SCHEME",
              env_or("DEFAULT_PASS_SCHEME", "SHA512-CRYPT"), "-");
  check_chars("DOMAIN", env_or("DOMAIN"), ".-");
  check_size("SIEVE_MAX_SCRIPT_SIZE", env_or("SIEVE_MAX_SCRIPT_SIZE", "500M"));
  {
    const std::string allow = env_or("DOVECOT_ALLOW_CLEARTEXT", "no");
    if (allow != "yes" && allow != "no")
      die_invalid("DOVECOT_ALLOW_CLEARTEXT", allow);
    const std::string dbg = env_or("DOVECOT_DEBUG_AUTH", "no");
    if (dbg != "yes" && dbg != "no")
      die_invalid("DOVECOT_DEBUG_AUTH", dbg);
    const std::string quota = env_or("DOVECOT_QUOTA", "no");
    if (quota != "yes" && quota != "no")
      die_invalid("DOVECOT_QUOTA", quota);
    const std::string mode = env_or("SPAM_DELIVERY_MODE", "reject");
    if (mode != "reject" && mode != "mark" && mode != "folder")
      die_invalid("SPAM_DELIVERY_MODE", mode);
  }

  if (argc > 1 && std::string(argv[1]) == "--healthcheck")
    return tcp_probe("127.0.0.1", 143);

  fs::create_directories(CONF_D);
  fs::create_directories(SIEVE_DIR);

  write_passdb();
  write_userdb();
  write_auth_cleartext();
  write_debug();
  write_spam_delivery_mode();
  write_sieve_limits();
  write_quota();
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
