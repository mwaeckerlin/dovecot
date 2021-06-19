#!/bin/sh -e

cat <<EOF >/etc/dovecot/dovecot-sql.conf
driver = mysql
connect = host=${DB_HOST} dbname=${DB_NAME} user=${DB_USER} password=${DB_PASSWORD}
password_query = select username,password from mailbox where local_part = '%n' and domain = '%d'
default_pass_scheme =  SHA512-CRYPT
EOF
chmod 600 /etc/dovecot/dovecot-sql.conf

dovecot -F
