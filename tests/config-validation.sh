#!/usr/bin/env bash
# Config validation: init must refuse malformed environment values.
#
# Every env value is rendered into a dovecot config file — the SQL passdb
# block, the quota dict, the cleartext/debug/sieve overrides. A value
# containing a newline would smuggle arbitrary extra directives into those
# files (config injection); an enum knob with a bogus value would produce a
# broken config. init therefore whitelist-validates every value up front and
# exits with a clear `invalid <VAR>` error before anything else runs.
#
# The image is shell-free, so the checks run from outside: the `--healthcheck`
# entrypoint path performs the same validation first, fails fast (no daemon is
# running) and never touches the network — `--network none` pins that.
# `--pull=never` keeps docker from testing a stale registry image.
#
# Usage: tests/config-validation.sh IMAGE

set -uo pipefail

IMAGE="${1:-mwaeckerlin/dovecot}"

PASS=0
FAIL=0
declare -a FAILED_NAMES

_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
_fail() { FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1"); echo "  FAIL  $1: $2"; }

_image_exists() {
    if docker image inspect "${IMAGE}" > /dev/null 2>&1; then
        return 0
    fi
    _fail "image_exists" "image not built — run 'npm run build' first"
    return 1
}

# A malformed value must abort with a message naming the variable.
_reject() {
    local name="$1" var="$2" value="$3"
    local out rc
    out=$(timeout 30 docker run --rm --pull=never --network none \
              -e "${var}=${value}" "${IMAGE}" --healthcheck 2>&1)
    rc=$?
    if [[ ${rc} -ne 0 && "${out}" == *"invalid ${var}"* ]]; then
        _pass "reject_${name}"
    else
        _fail "reject_${name}" "value not rejected (rc=${rc}): ${out}"
    fi
}

# A well-formed value must pass validation (the probe itself fails — no
# daemon is running — but no validation error may appear).
_accept() {
    local name="$1"
    shift
    local out
    out=$(timeout 30 docker run --rm --pull=never --network none \
              "$@" "${IMAGE}" --healthcheck 2>&1)
    if [[ "${out}" == *"invalid "* ]]; then
        _fail "accept_${name}" "valid value rejected: ${out}"
    else
        _pass "accept_${name}"
    fi
}

echo "==> Config validation: malformed environment must be refused"

_image_exists || { echo ""; echo "==> Config validation results: 0 passed, 1 failed"; exit 1; }

_reject db_password_injection DB_PASSWORD $'secret\npassword = stolen'
_reject db_user_injection     DB_USER     $'maildb\nhost = evil'
_reject db_host_bad           DB_HOST     "evil host"
_reject db_name_injection     DB_NAME     $'maildb\nquery = select 1'
_reject domain_bad            DOMAIN      $'example.com\nssl = no'
_reject pass_scheme_bad       DEFAULT_PASS_SCHEME "SHA512 CRYPT; drop"
_reject sieve_size_bad        SIEVE_MAX_SCRIPT_SIZE "500 megs"
_reject cleartext_bad         DOVECOT_ALLOW_CLEARTEXT "maybe"
_reject debug_bad             DOVECOT_DEBUG_AUTH "loud"
_reject quota_bad             DOVECOT_QUOTA "sometimes"
_reject spam_mode_bad         SPAM_DELIVERY_MODE "quarantine"

_accept defaults
_accept explicit_values \
    -e DB_USER=postfixadmin \
    -e DB_PASSWORD='s3cret-Pa55!' \
    -e DB_HOST=postfixadmin-db \
    -e DB_NAME=postfixadmin \
    -e DOMAIN=example.com \
    -e DEFAULT_PASS_SCHEME=SHA512-CRYPT \
    -e SIEVE_MAX_SCRIPT_SIZE=500M \
    -e DOVECOT_ALLOW_CLEARTEXT=no \
    -e DOVECOT_DEBUG_AUTH=no \
    -e DOVECOT_QUOTA=no \
    -e SPAM_DELIVERY_MODE=reject

echo ""
echo "==> Config validation results: ${PASS} passed, ${FAIL} failed"
if [[ ${FAIL} -gt 0 ]]; then
    echo "==> Failed checks: ${FAILED_NAMES[*]}"
    exit 1
fi
