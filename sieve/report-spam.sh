#!/bin/sh
# Bayes autotrainer wrapper — invoked by learn-spam.sieve via
# `sieve_pipe`. The mail body is on stdin, the recipient localpart on
# argv[1] (unused by rspamd's global classifier, kept for per-user).
# RSPAMD_HOST / RSPAMD_CTL_PORT come from the container env.
exec /usr/bin/rspamc \
    -h "${RSPAMD_HOST:-rspamd}:${RSPAMD_CTL_PORT:-11334}" \
    learn_spam
