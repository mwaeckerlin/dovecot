require ["vnd.dovecot.pipe", "copy", "imapsieve"];

# IMAPSieve trigger — this script runs whenever the user MOVEs or COPYs
# a message INTO the Junk folder from any other mailbox. The mail is
# piped verbatim to `rspamc learn_spam` inside the rspamd container.
# rspamd stores the tokens in Redis (see rspamd/classifier-bayes.conf),
# updating the Bayes classifier so future mail with the same tokens
# scores higher.
#
# The wrapper `report-spam` (installed at /usr/local/bin/report-spam)
# execs the rspamc client against RSPAMD_HOST / RSPAMD_CTL_PORT from
# the container env. Using a wrapper — rather than piping directly to
# rspamc — keeps the transport target changeable without touching the
# sieve script. No per-user argument: the classifier is global
# (per_user = false), so the recipient identity is irrelevant.

pipe :copy "report-spam";
