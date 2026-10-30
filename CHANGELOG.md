# Changelog

- 2026-10-30 **2.0.3**
    - `npm test` builds the image of this line before it measures it: all three lines build into the same tag, so a contract run without its own build measured whichever line was built last.
    - Sieve filters take effect again. The home of a mail user held his scripts in the root of his maildir, so Dovecot took the active script `.dovecot.sieve` for a mailbox, failed to open it and aborted every delivery that a rule touched. The message then reached INBOX through the implicit keep and no rule of the script took effect, which looks exactly like one single filter being broken. The home now lies under `/var/mail/domains/.sieve/<domain>/<localpart>`, inside the same volume and outside the mailbox namespace, and the start moves what an older image left in the maildir root, so no user loses a filter.
    - A webmail that showed a folder named `dovecot` with a subfolder `sieve` loses it with the same change: that entry was the script file.

- 2026-10-23 **2.0.2**
    - The version line 2 is built and published again as `mwaeckerlin/dovecot:2`, on every push to this branch and every week with the run on master, so it gets the security fixes of its base image
    - A test checks the image: the configuration is valid, the mailboxes stay in the `<domain>/<localpart>` layout, and a started container takes the certificate and the database settings and answers on IMAP, POP3, LMTP, ManageSieve and the SASL port of postfix
