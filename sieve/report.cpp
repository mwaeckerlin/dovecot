/**

Bayes autotrainer wrapper — invoked by learn-spam.sieve /
learn-ham.sieve via `sieve_pipe`. The mail body arrives on stdin and
is passed through to rspamc (exec inherits the file descriptors).

One static binary, installed twice: as /usr/local/bin/report-spam and
/usr/local/bin/report-ham. The basename decides the rspamc command —
exactly like the former report-spam.sh / report-ham.sh shell wrappers,
which cannot exist in the headless image (no shell).

RSPAMD_HOST / RSPAMD_CTL_PORT come from the container env, which
dovecot passes through to sieve_pipe children.

*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

int main(int, char *argv[]) {
  const char *host = std::getenv("RSPAMD_HOST");
  if (!host || !*host) host = "rspamd";
  const char *port = std::getenv("RSPAMD_CTL_PORT");
  if (!port || !*port) port = "11334";
  const std::string hostport = std::string(host) + ":" + port;

  const char *base = std::strrchr(argv[0], '/');
  base = base ? base + 1 : argv[0];
  const char *cmd =
      std::strcmp(base, "report-ham") == 0 ? "learn_ham" : "learn_spam";

  execl("/usr/bin/rspamc", "rspamc", "-h", hostport.c_str(), cmd,
        static_cast<char *>(nullptr));
  std::perror("/usr/bin/rspamc");
  return 1;
}
