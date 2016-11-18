#!/bin/bash -ex

# Configure LDAP
if test -n "$LDAP_ENV_DOMAIN" -a -z "$LDAP_HOST"; then
    LDAP_HOST="ldap"
fi
LDAP_DOMAIN=${LDAP_DOMAIN:-${LDAP_ENV_DOMAIN}}
LDAP_HOST=${LDAP_HOST:-$LDAP_DOMAIN}
LDAP_BASE=dc=${LDAP_BASE:-${LDAP_DOMAIN//./,dc=}}
LDAP_URL=${LDAP_URL:-ldap://${LDAP_HOST}:389}
if test -n "$LDAP_BIND_DN"; then
    LDAP_BIND_DN=${LDAP_BIND_DN},${LDAP_BASE}
fi
sed -i \
    -e 's|LDAP_HOST|'"$LDAP_HOST"'|' \
    -e 's|LDAP_URL|'"$LDAP_URL"'|' \
    -e 's|LDAP_BIND_DN|'"$LDAP_BIND_DN"'|' \
    -e 's|LDAP_BIND_PWD|'"$LDAP_BIND_PWD"'|' \
    -e 's|LDAP_BASE|'"$LDAP_BASE"'|' \
    /etc/dovecot/dovecot-ldap.conf

dovecot -F
