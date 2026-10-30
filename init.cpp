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
     SHA512-CRYPT (PostfixAdmin's php_crypt). DB_PORT and DB_SSL add a
     `port` / `ssl` line only when set.
  2. conf.d/10-connection-limits.conf: mail_max_userip_connections
     from DOVECOT_MAX_USERIP_CONNECTIONS (default 1000) and, for
     ManageSieve, DOVECOT_SIEVE_MAX_USERIP_CONNECTIONS (default 100).
  3. conf.d/10-auth-cleartext.conf: auth_allow_cleartext from
     DOVECOT_ALLOW_CLEARTEXT (default no — cleartext mechanisms only
     over TLS, a password never travels unencrypted).
  4. conf.d/10-debug.conf (log_debug=category=auth) only when
     DOVECOT_DEBUG_AUTH=yes — a diagnostics override, never a
     production default.
  5. sieve/spam-to-junk.sieve according to SPAM_DELIVERY_MODE
     (reject / mark / folder) and pre-compiled with sievec — the mail
     user executing it at delivery time has no write access to
     /etc/dovecot/sieve. A compile error is a hard startup failure.
  6. conf.d/10-ssl.conf when /etc/letsencrypt/live/$DOMAIN/ holds
     fullchain.pem + privkey.pem.
  7. exec dovecot -F.

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

// The home of a mail user holds his personal Sieve scripts. It must not
// be the root of his maildir: when Dovecot creates a mailbox it walks
// that directory and takes the active script `.dovecot.sieve` for a
// mailbox of its own, fails on `stat(.dovecot.sieve/tmp)` with ENOTDIR
// and aborts the whole delivery with a temporary error. Postfix then
// defers the message and no rule of the script takes effect. Measured
// on 2026-10-28 against Dovecot 2.4.5 / Pigeonhole 2.4.5, pinned by
// tests/e2e/test_sieve.py::test_sieve_fileinto_creates_a_missing_folder.
// The new home stays INSIDE the persisted volume (`VOLUME
// /var/mail/domains`): a path beside it would live in the writable
// layer of the container and every restart would drop every filter.
// `.sieve` sits at the level of the domains, which Dovecot never scans
// for mailboxes — only the directory of a single user is a mailbox root.
constexpr const char *MAIL_ROOT   = "/var/mail/domains";
constexpr const char *SIEVE_HOMES = "/var/mail/domains/.sieve";
constexpr uid_t MAIL_UID = 5000;
constexpr gid_t MAIL_GID = 5000;

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

void
check_num(const char *var, const std::string &v, long min, long max) {
  if (v.empty() || v.size() > 9 ||
      v.find_first_not_of("0123456789") != std::string::npos)
    die_invalid(var, v);
  const long n = std::atol(v.c_str());
  if (n < min || n > max) die_invalid(var, v);
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
  const std::string port = env_or("DB_PORT");
  const std::string ssl  = env_or("DB_SSL");
  const std::string content =
      "mysql maildb {\n"
      "  host = "     + env_or("DB_HOST")     + "\n"
      + (port.empty() ? "" : "  port = " + port + "\n") +
      "  dbname = "   + env_or("DB_NAME")     + "\n"
      "  user = "     + env_or("DB_USER")     + "\n"
      "  password = " + env_or("DB_PASSWORD") + "\n"
      + (ssl.empty() ? "" : "  ssl = " + ssl + "\n") +
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

// Sieve can send mail of its own: `reject` answers the sender with a
// rejection, `vacation` replies, `redirect` forwards. This image gives
// it no way out, and that is the design, not an omission: a message
// that reached delivery was accepted at SMTP time, and the envelope
// sender of unwanted mail is forged, so a rejection generated here
// would travel to an innocent address. That is backscatter, and it gets
// the sending host listed on the blocklists it belongs on.
//
// Two rules decide it (Marc, 2026-10-28): everything accepted is
// delivered, and everything suspected of being spam is refused before
// acceptance. The refusal happens in the rspamd milter during the SMTP
// transaction, where the sending server still holds the message and its
// own provider informs the real sender.
//
// So `submission_host` stays unset, Pigeonhole finds no sendmail, a
// `reject` in a user script sends nothing, and the implicit keep puts
// the message in INBOX — delivered, as the first rule requires. The
// knob exists for a deployment that wants `vacation` or `redirect`, and
// whoever sets it takes the backscatter with it.
void
write_submission_host() {
  const std::string host = env_or("DOVECOT_SUBMISSION_HOST");
  if (host.empty()) return;
  check_chars("DOVECOT_SUBMISSION_HOST", host, ".-_:");
  write_file(fs::path(CONF_D) / "10-submission-host.conf",
             "submission_host = " + host + "\n");
  std::cerr << "**** submission_host = " << host
            << " — sieve reject, vacation and redirect now send mail;"
            << " a forged envelope sender receives it" << std::endl;
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

// A Sieve script the operator writes, running before the script of
// every user and surviving every save in the webmail: SnappyMail
// manages the one active script over ManageSieve and never sees this
// one. It routes mail for all mailboxes at once — `fileinto "Junk"`,
// `discard`, `stop`.
//
// It must not `reject`. A reject runs at delivery time, when the
// message is already accepted, so it can only answer the address in
// MAIL FROM, which unwanted mail forges. A rule that refuses belongs
// in Postfix, where `header_checks` answer the sending host with 5xx
// during the SMTP transaction and this server sends nothing.
//
// Empty by default, so nothing changes for an existing deployment. A
// compile error aborts the start: a filter that silently does nothing
// is the defect this whole mechanism exists to avoid.
void
write_admin_sieve() {
  const std::string script = env_or("SIEVE_ADMIN_SCRIPT");
  const fs::path conf = fs::path(CONF_D) / "20-admin-sieve.conf";
  const fs::path sieve = fs::path(SIEVE_DIR) / "admin-before.sieve";
  if (script.empty()) {
    std::error_code ec;
    fs::remove(conf, ec);
    fs::remove(sieve, ec);
    fs::remove(fs::path(sieve) += "c", ec);
    return;
  }
  if (script.find("reject") != std::string::npos) {
    std::cerr << "**** ERROR: SIEVE_ADMIN_SCRIPT contains `reject`, which"
              << " would answer the forged sender address of unwanted mail."
              << " Use `discard` or `fileinto \"Junk\"`, or refuse the"
              << " message in Postfix with REJECT_SHOUTING_SUBJECT and the"
              << " other header rules." << std::endl;
    std::exit(1);
  }
  write_file(sieve, script.back() == '\n' ? script : script + "\n");
  std::string out;
  if (run_capture({SIEVEC, sieve.c_str()}, out) != 0)
    throw std::runtime_error("SIEVE_ADMIN_SCRIPT does not compile: " + out);
  write_file(conf,
             "sieve_script admin-before {\n"
             "  type = before\n"
             "  path = " + sieve.string() + "\n"
             "}\n");
  std::cerr << "**** SIEVE_ADMIN_SCRIPT active, runs before every user script"
            << std::endl;
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

// Written to conf.d so it wins over local.conf (included before it).
void
write_connection_limits() {
  const std::string all   = env_or("DOVECOT_MAX_USERIP_CONNECTIONS", "1000");
  const std::string sieve = env_or("DOVECOT_SIEVE_MAX_USERIP_CONNECTIONS", "100");
  write_file(fs::path(CONF_D) / "10-connection-limits.conf",
             "mail_max_userip_connections = " + all + "\n"
             "\n"
             "protocol sieve {\n"
             "  mail_max_userip_connections = " + sieve + "\n"
             "}\n");
  std::cerr << "**** mail_max_userip_connections = " << all
            << " (ManageSieve " << sieve << ")" << std::endl;
}

// The userdb provides uid/gid/home for every mailbox. It lives here (not
// in local.conf) so DOVECOT_QUOTA can switch its backend without a
// second, non-merging userdb block:
//   off  → static (uid/gid/home only; identical to the previous default)
//   on   → sql, returning uid/gid/home AND the per-user quota limit from
//          the PostfixAdmin `mailbox.quota` column (bytes; <=0 =
//          unlimited → quota_storage_size stays NULL, user unaffected).
// Move the Sieve scripts an older image left in the maildir root into
// the separate home, once, so an upgrade keeps every filter a user had.
// `.dovecot.sieve` is normally a relative symlink into `sieve/`, so both
// names travel together and the link stays intact.
void
migrate_sieve_homes() {
  if (!fs::is_directory(MAIL_ROOT)) return;
  std::error_code ec;
  for (const auto &domain : fs::directory_iterator(MAIL_ROOT, ec)) {
    if (!domain.is_directory()) continue;
    if (domain.path().filename() == ".sieve") continue;
    for (const auto &user : fs::directory_iterator(domain.path(), ec)) {
      if (!user.is_directory()) continue;
      // A symbolic link from one mailbox to another is how an alias was
      // built before PostfixAdmin owned them (measured 2026-10-28:
      // atheismus.ch/atheismus -> info). Following it would move the
      // scripts of the target into the home of the alias, and the target
      // would then find nothing and lose every filter.
      if (fs::is_symlink(fs::symlink_status(user.path(), ec))) continue;
      const fs::path target = fs::path(SIEVE_HOMES)
          / domain.path().filename() / user.path().filename();
      for (const char *name : {"sieve", ".dovecot.sieve",
                               ".dovecot.sieve.log"}) {
        const fs::path from = user.path() / name;
        if (!fs::exists(fs::symlink_status(from, ec))) continue;
        const fs::path to = target / name;
        if (fs::exists(fs::symlink_status(to, ec))) continue;
        fs::create_directories(target, ec);
        ::chown(target.c_str(), MAIL_UID, MAIL_GID);
        ::chown(target.parent_path().c_str(), MAIL_UID, MAIL_GID);
        fs::rename(from, to, ec);
        if (ec) {
          std::cerr << "**** ERROR: cannot move " << from << " -> " << to
                    << ": " << ec.message() << std::endl;
          ec.clear();
          continue;
        }
        std::cerr << "**** moved " << from << " -> " << to << std::endl;
      }
    }
  }
  fs::create_directories(SIEVE_HOMES, ec);
  ::chown(SIEVE_HOMES, MAIL_UID, MAIL_GID);
}

void
write_userdb() {
  const std::string home =
      std::string(SIEVE_HOMES) + "/%{user | domain}/%{user | username}";
  std::string content;
  if (env_or("DOVECOT_QUOTA", "no") == "yes") {
    content =
        "userdb sql {\n"
        "  sql_driver = mysql\n"
        "  query = SELECT 5000 AS uid, 5000 AS gid, "
        "CONCAT('" + std::string(SIEVE_HOMES) + "/', "
        "SUBSTRING_INDEX(username,'@',-1), "
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
  if (!env_or("DB_PORT").empty()) check_num("DB_PORT", env_or("DB_PORT"), 1, 65535);
  check_num("DOVECOT_MAX_USERIP_CONNECTIONS",
            env_or("DOVECOT_MAX_USERIP_CONNECTIONS", "1000"), 1, 1000000);
  check_num("DOVECOT_SIEVE_MAX_USERIP_CONNECTIONS",
            env_or("DOVECOT_SIEVE_MAX_USERIP_CONNECTIONS", "100"), 1, 1000000);
  {
    const std::string ssl = env_or("DB_SSL");
    if (!ssl.empty() && ssl != "yes" && ssl != "no")
      die_invalid("DB_SSL", ssl);
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
  write_connection_limits();
  migrate_sieve_homes();
  write_userdb();
  write_auth_cleartext();
  write_submission_host();
  write_debug();
  write_spam_delivery_mode();
  write_admin_sieve();
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
