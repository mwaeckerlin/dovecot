#!/bin/sh -e

mkdir -p /etc/dovecot/conf.d

cat <<EOF >/etc/dovecot/conf.d/passdb-sql.conf
mysql maildb {
  host = ${DB_HOST}
  dbname = ${DB_NAME}
  user = ${DB_USER}
  password = ${DB_PASSWORD}
}

passdb sql {
  sql_driver = mysql
  query = SELECT password FROM mailbox WHERE username = '%{user}'
  default_password_scheme = ${DEFAULT_PASS_SCHEME:-SHA512-CRYPT}
}
EOF
chmod 600 /etc/dovecot/conf.d/passdb-sql.conf

# SPAM_DELIVERY_MODE — write the sieve_before script that decides how
# mail carrying `X-Spam-Flag: YES` (added by rspamd milter_headers
# above the add-header score, but not above the reject score) is
# delivered. Default is `reject` (nothing above reject score reaches
# here — this file is then a no-op that just documents the mode).
mkdir -p /etc/dovecot/sieve
case "${SPAM_DELIVERY_MODE:-reject}" in
    folder)
        cat >/etc/dovecot/sieve/spam-to-junk.sieve <<'EOF'
require ["fileinto", "mailbox"];
# SPAM_DELIVERY_MODE=folder — deliver spam to the Junk folder.
if header :contains "X-Spam-Flag" "YES" {
    fileinto :create "Junk";
    stop;
}
EOF
        echo "**** SPAM_DELIVERY_MODE=folder — spam → Junk"
        ;;
    mark)
        cat >/etc/dovecot/sieve/spam-to-junk.sieve <<'EOF'
# SPAM_DELIVERY_MODE=mark — rspamd already added X-Spam-Flag / Level /
# Status headers upstream (see rspamd/milter_headers.conf). Nothing to
# do at delivery time; the user's MUA filters on the headers.
EOF
        echo "**** SPAM_DELIVERY_MODE=mark — X-Spam-* headers only, INBOX delivery"
        ;;
    reject|*)
        cat >/etc/dovecot/sieve/spam-to-junk.sieve <<'EOF'
# SPAM_DELIVERY_MODE=reject — rspamd rejects everything above
# RSPAMD_REJECT_SCORE at SMTP time; nothing reaches this sieve.
EOF
        echo "**** SPAM_DELIVERY_MODE=reject — SMTP-time rejection only"
        ;;
esac
# Pre-compile: the mail user executing the script at delivery time
# has no write access to /etc/dovecot/sieve, so the compiled .svbin
# must exist up front (root compiles it here). A compile error is a
# hard startup failure — never silently deliver without the filter.
sievec /etc/dovecot/sieve/spam-to-junk.sieve

if test -e /etc/letsencrypt/live/${DOMAIN}/fullchain.pem \
    -a -e /etc/letsencrypt/live/${DOMAIN}/privkey.pem; then
    cat <<EOF >/etc/dovecot/conf.d/10-ssl.conf
ssl = yes
ssl_server_cert_file = /etc/letsencrypt/live/${DOMAIN}/fullchain.pem
ssl_server_key_file = /etc/letsencrypt/live/${DOMAIN}/privkey.pem
ssl_server_prefer_ciphers = server
EOF
    echo "**** TLS configured for ${DOMAIN}"
fi

dovecot -F
