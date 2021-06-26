#!/bin/sh -e

cat <<EOF >/etc/dovecot/dovecot-sql.conf
driver = mysql
connect = host=${DB_HOST} dbname=${DB_NAME} user=${DB_USER} password=${DB_PASSWORD}
password_query = select username,password from mailbox where local_part = '%n' and domain = '%d'
default_pass_scheme =  SHA512-CRYPT
EOF
chmod 600 /etc/dovecot/dovecot-sql.conf

if test -e /etc/letsencrypt/live/${DOMAIN}/fullchain.pem \
    -a -e /etc/letsencrypt/live/${DOMAIN}/privkey.pem; then
    cat <<EOF >/etc/dovecot/conf.d/10-ssl.conf
ssl = required
ssl_cert = </etc/letsencrypt/live/${DOMAIN}/fullchain.pem
ssl_key = </etc/letsencrypt/live/${DOMAIN}/privkey.pem
ssl_prefer_server_ciphers = yes
EOF
    echo "**** TLS configured for ${DOMAIN}}"
fi

dovecot -F
