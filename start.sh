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

# The home of a mail user holds his personal Sieve scripts. It must not
# be the root of his maildir: when Dovecot creates a mailbox it walks
# that directory, takes the active script `.dovecot.sieve` for a mailbox
# of its own, fails on `stat(.dovecot.sieve/tmp)` with ENOTDIR and
# aborts the whole delivery with a temporary error. Postfix then defers
# the message and no rule of the script takes effect. Measured on
# 2026-10-28 against Dovecot 2.4.5 / Pigeonhole 2.4.5.
#
# The scripts move once into a home beside the mail, inside the same
# volume (`VOLUME /var/mail/domains`) and outside the mailbox namespace:
# the glob skips `.sieve` because it starts with a dot. `.dovecot.sieve`
# is a relative symlink into `sieve/`, so both names travel together and
# the link stays intact. A mailbox reached through a symlink is skipped:
# following it would move the scripts of the target into the home of the
# alias, and the target would lose every filter.
SIEVE_HOMES=/var/mail/domains/.sieve
for domain in /var/mail/domains/*; do
    if test -d "${domain}" -a ! -L "${domain}"; then
        for user in "${domain}"/*; do
            if test -d "${user}" -a ! -L "${user}"; then
                target="${SIEVE_HOMES}/${domain##*/}/${user##*/}"
                for name in sieve .dovecot.sieve .dovecot.sieve.log; do
                    if test \( -e "${user}/${name}" -o -L "${user}/${name}" \) \
                            -a ! -e "${target}/${name}"; then
                        mkdir -p "${target}"
                        chown 5000:5000 "${SIEVE_HOMES}" \
                              "${SIEVE_HOMES}/${domain##*/}" "${target}"
                        mv "${user}/${name}" "${target}/${name}"
                        echo "**** moved ${user}/${name} -> ${target}/${name}"
                    fi
                done
            fi
        done
    fi
done
mkdir -p "${SIEVE_HOMES}"
chown 5000:5000 "${SIEVE_HOMES}"

dovecot -F
