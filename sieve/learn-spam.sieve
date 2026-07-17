require ["vnd.dovecot.pipe", "copy", "imapsieve", "environment", "variables"];

# IMAPSieve trigger — this script runs whenever the user MOVEs or COPYs
# a message INTO the Junk folder from any other mailbox. The mail is
# piped verbatim to `rspamc learn_spam` inside the rspamd container.
# rspamd stores the tokens in Redis (see rspamd/classifier-bayes.conf),
# updating the Bayes classifier so future mail with the same tokens
# scores higher.
#
# The wrapper `report-spam` (installed at /usr/local/bin/report-spam)
# is a tiny shell script that execs the rspamc client with the correct
# host from the RSPAMD_HOST / RSPAMD_CTL_PORT env baked in at start-up.
# Using a wrapper — rather than piping directly to rspamc — keeps the
# transport target changeable without editing every user's sieve
# script.

if environment :matches "imap.email" "*" {
  set "email" "${1}";
}

pipe :copy "report-spam" [ "${email}" ];
