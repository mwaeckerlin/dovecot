# Dovecot Docker Image

Docker Image for Dovecot IMAP/POP3/ManageSieve Server.

Set your domain in the `DOMAIN` variable and provide the Let's Encrypt
certificate at `/etc/letsencrypt/live/$DOMAIN/` (fullchain.pem +
privkey.pem) so TLS is configured. With a cert present, dovecot serves
IMAPS (993), POP3S (995) and STARTTLS on the plain ports.

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
