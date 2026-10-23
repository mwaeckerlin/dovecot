# Changelog

- 2026-10-23 **2.0.2**
    - The version line 2 is built and published again as `mwaeckerlin/dovecot:2`, on every push to this branch and every week with the run on master, so it gets the security fixes of its base image
    - A test checks the image: the configuration is valid, the mailboxes stay in the `<domain>/<localpart>` layout, and a started container takes the certificate and the database settings and answers on IMAP, POP3, LMTP, ManageSieve and the SASL port of postfix
