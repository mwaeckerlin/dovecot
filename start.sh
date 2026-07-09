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
