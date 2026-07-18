# Dovecot Docker Image

Docker Image for Dovecot IMAP/POP3/ManageSieve Server.

Set your domain in the `DOMAIN` variable and provide the Let's Encrypt
certificate at `/etc/letsencrypt/live/$DOMAIN/` (fullchain.pem +
privkey.pem) so TLS is configured. With a cert present, dovecot serves
IMAPS (993), POP3S (995) and STARTTLS on the plain ports.

## Headless image

The image is headless: a small compiled `init` binary writes the
runtime configuration (SQL passdb, TLS, auth policy, spam delivery
sieve) into `/etc/dovecot/conf.d/` from the environment and execs the
dovecot daemon — no shell, no busybox, no package manager in the
shipped image. The Bayes autotrainer wrappers (`report-spam` /
`report-ham`, invoked by the IMAPSieve learn scripts) are one static
binary that execs `rspamc`. `init --healthcheck` TCP-probes the IMAP
listener on 127.0.0.1:143 and can be wired as a Docker healthcheck:

```yaml
healthcheck:
  test: ["CMD", "/usr/bin/init", "--healthcheck"]
```

## Authentication over TLS (secure default)

`DOVECOT_ALLOW_CLEARTEXT` (default **`no`**) controls whether cleartext
login mechanisms (PLAIN/LOGIN) may be used on an **unencrypted**
connection:

- `no` (default): cleartext auth is offered **only over TLS**. A client
  must use IMAPS/POP3S or issue STARTTLS before authenticating, so a
  password is never sent in the clear. This requires a TLS certificate
  to be present — without a cert *and* without softening this setting
  there is no usable auth at all, which is the intended secure default.
- `yes`: allow cleartext logins on unencrypted connections. Soften to
  this **only deliberately** (e.g. a trusted, isolated network without
  certificates).

## Environment variables

| Variable                  | Default          | Description                                                      |
|---------------------------|------------------|------------------------------------------------------------------|
| `DOMAIN`                  | —                | Mail domain; also the Let's Encrypt cert directory name.         |
| `DB_HOST` / `DB_NAME` / `DB_USER` / `DB_PASSWORD` | — | PostfixAdmin database connection for the passdb.                 |
| `DEFAULT_PASS_SCHEME`     | `SHA512-CRYPT`   | Password scheme dovecot expects in the `mailbox.password` column (matches PostfixAdmin's `php_crypt`). |
| `DOVECOT_ALLOW_CLEARTEXT` | `no`             | Allow cleartext auth on unencrypted connections. See above.      |
| `RSPAMD_HOST` / `RSPAMD_CTL_PORT` | `rspamd` / `11334` | Rspamd controller the IMAPSieve Bayes autotrainer pipes to (`rspamc learn_spam/learn_ham`). |
| `SPAM_DELIVERY_MODE`      | `reject`         | `reject` / `mark` / `folder` — how spam-tagged mail is delivered (see mailservice README). |
| `DOVECOT_DEBUG_AUTH`      | `no`             | Diagnostics override: `yes` enables verbose auth debug logging (`log_debug = category=auth`). Never a production default — debug lines expose per-login details to every log reader. |

## Security trade-offs

- **Container runs as root:** the dovecot master process needs root to
  bind its service listeners and to spawn per-login workers; every
  IMAP/POP3 session, sieve execution and mail delivery then runs as the
  unprivileged mail user (uid/gid 5000). This deviates from the
  image family's non-root default deliberately.
- **Internal listeners without authentication:** LMTP (port 24) and the
  SASL socket for postfix (port 12345) trust the docker network they
  are attached to. Never publish these ports; only attach services to
  the shared networks that genuinely need them.
- **Auth debug logging** is off by default (`DOVECOT_DEBUG_AUTH=no`),
  see the environment table above.
