require ["vnd.dovecot.pipe", "copy", "imapsieve", "environment", "variables"];

# IMAPSieve trigger — this script runs whenever the user MOVEs or COPYs
# a message OUT of the Junk folder (i.e. tells the mail server «this
# was NOT spam»). The mail is piped to `rspamc learn_ham`, unlearning
# the tokens and, over time, lowering the Bayes score of similar mail.

if environment :matches "imap.email" "*" {
  set "email" "${1}";
}

pipe :copy "report-ham" [ "${email}" ];
