#!/bin/sh
# Bayes autotrainer wrapper — invoked by learn-ham.sieve via
# `sieve_pipe`. See report-spam.sh for the general shape.
exec /usr/bin/rspamc \
    -h "${RSPAMD_HOST:-rspamd}:${RSPAMD_CTL_PORT:-11334}" \
    learn_ham
