#!/usr/bin/env bash
# Environment contract: every operator setting reaches the effective
# dovecot configuration — the image default when the variable is unset,
# the operator's value when it is set.
#
# Each case starts the image with its environment, waits until init has
# written conf.d and dovecot listens (the --healthcheck probe answers),
# and reads the configuration dovecot itself parsed: `doveconf -h` for a
# global setting, the matching block of `doveconf -n` for the SQL
# connection and the ManageSieve protocol. `--network none` keeps the
# container off every network (the database is never contacted at
# start-up); `--pull=never` tests the local build.
#
# Usage: tests/env-config.sh IMAGE

set -uo pipefail

IMAGE="${1:-mwaeckerlin/dovecot}"

PASS=0
FAIL=0
declare -a FAILED_NAMES
declare -a CONTAINERS

_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
_fail() { FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1"); echo "  FAIL  $1: $2"; }

_cleanup() {
    [[ ${#CONTAINERS[@]} -gt 0 ]] && docker rm -f "${CONTAINERS[@]}" > /dev/null 2>&1
}
trap _cleanup EXIT

_image_exists() {
    if docker image inspect "${IMAGE}" > /dev/null 2>&1; then
        return 0
    fi
    _fail "image_exists" "image not built — run 'npm run build' first"
    return 1
}

# Start the image with the given docker arguments; sets CID once init is
# done and the IMAP listener answers, fails after 60 probes.
_start() {
    local name="$1" i
    shift
    CID=$(docker run -d --pull=never --network none \
              -e DB_HOST=postfixadmin-db -e DB_NAME=postfixadmin \
              -e DB_USER=postfixadmin -e DB_PASSWORD=test-only \
              "$@" "${IMAGE}")
    CONTAINERS+=("${CID}")
    for i in $(seq 1 60); do
        if docker exec "${CID}" /usr/bin/init --healthcheck > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    _fail "${name}_started" "listener never came up: $(docker logs "${CID}" 2>&1)"
    return 1
}

_expect_global() {
    local name="$1" setting="$2" want="$3" got
    got=$(docker exec "${CID}" /usr/bin/doveconf -h "${setting}" 2>/dev/null)
    if [[ "${got}" == "${want}" ]]; then
        _pass "${name}"
    else
        _fail "${name}" "${setting} = '${got}', expected '${want}'"
    fi
}

# The lines of every `doveconf -n` block that opens with the given header.
_block() {
    docker exec "${CID}" /usr/bin/doveconf -n 2>/dev/null | awk -v head="$1" '
        $0 ~ "^[[:space:]]*" head " [{]" { inside = 1; next }
        inside && /^[[:space:]]*[}]/     { inside = 0; next }
        inside                           { print }'
}

# The block holds a line matching the extended regular expression.
_expect_in_block() {
    local name="$1" head="$2" pattern="$3" block
    block=$(_block "${head}")
    if grep -qE "${pattern}" <<< "${block}"; then
        _pass "${name}"
    else
        _fail "${name}" "no line /${pattern}/ in «${head}»: ${block}"
    fi
}

# The block holds no line matching the extended regular expression.
_expect_not_in_block() {
    local name="$1" head="$2" pattern="$3" block
    block=$(_block "${head}")
    if [[ -n "${block}" ]] && ! grep -qE "${pattern}" <<< "${block}"; then
        _pass "${name}"
    else
        _fail "${name}" "line /${pattern}/ present or block «${head}» missing: ${block}"
    fi
}

echo "==> Environment contract: settings reach the effective configuration"

_image_exists || { echo ""; echo "==> Environment contract results: 0 passed, 1 failed"; exit 1; }

if _start defaults; then
    _expect_global   default_userip mail_max_userip_connections "1000"
    _expect_in_block default_sieve_userip "protocol sieve" '^[[:space:]]*mail_max_userip_connections = 100$'
    _expect_not_in_block default_db_port "mysql maildb" '^[[:space:]]*(mysql_)?port = '
    _expect_not_in_block default_db_ssl  "mysql maildb" '^[[:space:]]*(mysql_)?ssl = '
fi

if _start override \
       -e DOVECOT_MAX_USERIP_CONNECTIONS=555 \
       -e DOVECOT_SIEVE_MAX_USERIP_CONNECTIONS=77 \
       -e DB_PORT=3307 \
       -e DB_SSL=no; then
    _expect_global   override_userip mail_max_userip_connections "555"
    _expect_in_block override_sieve_userip "protocol sieve" '^[[:space:]]*mail_max_userip_connections = 77$'
    _expect_in_block override_db_port "mysql maildb" '^[[:space:]]*(mysql_)?port = 3307$'
    _expect_in_block override_db_ssl  "mysql maildb" '^[[:space:]]*(mysql_)?ssl = no$'
fi

echo ""
echo "==> Environment contract results: ${PASS} passed, ${FAIL} failed"
if [[ ${FAIL} -gt 0 ]]; then
    echo "==> Failed checks: ${FAILED_NAMES[*]}"
    exit 1
fi
