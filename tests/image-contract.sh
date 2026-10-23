#!/usr/bin/env bash
# Image contract of the version line 2: the image keeps the configuration and
# the start-up behaviour this line has always had.
#
#   - `doveconf -n` accepts the configuration, and the mailboxes stay in the
#     maildir layout <domain>/<localpart> under /var/mail/domains
#   - started with a certificate and the database settings, start.sh writes
#     the SQL passdb and the TLS configuration, dovecot accepts the result and
#     answers on IMAP (143, with STARTTLS), POP3 (110), LMTP (24), ManageSieve
#     (4190) and on the SASL port 12345 that postfix asks for PLAIN and LOGIN
#
# Nothing leaves the runner: the database is named but never reached.
#
# Usage: tests/image-contract.sh IMAGE

set -uo pipefail

# the last argument, so `npm test -- <image>` measures another image
IMAGE="${!#}"
NAME="dovecot-contract-$$"
WORK="$(mktemp -d)"
PASS=0
FAIL=0
declare -a FAILED_NAMES

_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
_fail() { FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1"); echo "  FAIL  $1: $2"; }

_cleanup() {
    docker rm -f "${NAME}" > /dev/null 2>&1
    rm -rf "${WORK}"
}
trap _cleanup EXIT

_expect_setting() {
    local key="$1" expected="$2" actual
    actual="$(docker run --rm --pull=never --entrypoint /usr/bin/doveconf "${IMAGE}" -h "${key}" 2>&1)"
    if [[ "${actual}" == "${expected}" ]]; then
        _pass "setting_${key}"
    else
        _fail "setting_${key}" "expected '${expected}', found '${actual}'"
    fi
}

# the lines a service sends on connect, up to the one that matches $3
_greeting() {
    local port="$1" until="$2" address line
    address="$(docker port "${NAME}" "${port}/tcp")"
    exec 3<> "/dev/tcp/${address%:*}/${address##*:}" || return 1
    while read -t 10 -r line <&3; do
        echo "${line}"
        [[ "${line}" =~ ${until} ]] && break
    done
    exec 3<&-
}

# waits until the service on $1 sends a line that matches $2
_wait_for() {
    local port="$1" until="$2" answer=""
    for _ in $(seq 1 60); do
        answer="$(_greeting "${port}" "${until}" 2> /dev/null)"
        [[ "${answer}" =~ ${until} ]] && break
        sleep 1
    done
    echo "${answer}"
}

_expect_answer() {
    local name="$1" port="$2" pattern="$3" answer
    answer="$(_wait_for "${port}" "${pattern}")"
    if [[ "${answer}" =~ ${pattern} ]]; then
        _pass "${name}"
    else
        _fail "${name}" "port ${port} answered '${answer:-nothing}', expected ${pattern} / log: $(docker logs "${NAME}" 2>&1)"
    fi
}

echo "==> Image contract: ${IMAGE}"

if ! docker image inspect "${IMAGE}" > /dev/null 2>&1; then
    _fail "image_exists" "image not built — run 'npm run build' first"
    echo "==> Image contract results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi

CONFIG="$(docker run --rm --pull=never --entrypoint /usr/bin/doveconf "${IMAGE}" -n 2>&1)"
if [[ $? -eq 0 ]]; then
    _pass "doveconf_image"
else
    _fail "doveconf_image" "${CONFIG}"
fi

_expect_setting mail_driver "maildir"
_expect_setting mail_path "/var/mail/domains/%{user | domain}/%{user | username}"
_expect_setting protocols "imap pop3 lmtp sieve"

mkdir -p "${WORK}/letsencrypt/live/mail.example.com"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=mail.example.com" \
    -keyout "${WORK}/letsencrypt/live/mail.example.com/privkey.pem" \
    -out "${WORK}/letsencrypt/live/mail.example.com/fullchain.pem" > /dev/null 2>&1

docker create --name "${NAME}" --pull=never \
    -p 127.0.0.1::143 -p 127.0.0.1::110 -p 127.0.0.1::24 -p 127.0.0.1::4190 -p 127.0.0.1::12345 \
    -e DOMAIN=mail.example.com \
    -e DB_HOST=db.example.com -e DB_NAME=postfix -e DB_USER=postfix -e DB_PASSWORD=contract-secret \
    "${IMAGE}" > /dev/null
docker cp "${WORK}/letsencrypt/." "${NAME}:/etc/letsencrypt" > /dev/null
docker start "${NAME}" > /dev/null

_expect_answer imap_starttls 143 '^\* OK .*STARTTLS'
_expect_answer pop3_greeting 110 '^\+OK'
_expect_answer lmtp_greeting 24 '^220 '
_expect_answer managesieve_greeting 4190 '^OK '
SASL="$(_wait_for 12345 '^DONE')"
if [[ "${SASL}" == *$'MECH\tPLAIN'* && "${SASL}" == *$'MECH\tLOGIN'* ]]; then
    _pass "sasl_plain_login"
else
    _fail "sasl_plain_login" "the SASL port offers: ${SASL:-nothing}"
fi

LOG="$(docker logs "${NAME}" 2>&1)"
if [[ "${LOG}" == *"TLS configured for mail.example.com"* ]]; then
    _pass "tls_configured"
else
    _fail "tls_configured" "the certificate was not taken: ${LOG}"
fi

RUNTIME="$(docker exec "${NAME}" doveconf -n 2>&1)"
if [[ $? -eq 0 && "${RUNTIME}" == *"db.example.com"* && "${RUNTIME}" == *"ssl_server_cert_file = /etc/letsencrypt/live/mail.example.com/fullchain.pem"* ]]; then
    _pass "doveconf_runtime"
else
    _fail "doveconf_runtime" "${RUNTIME}"
fi

echo ""
echo "==> Image contract results: ${PASS} passed, ${FAIL} failed"
if [[ ${FAIL} -gt 0 ]]; then
    echo "==> Failed contracts: ${FAILED_NAMES[*]}"
    exit 1
fi
