# Changelog

- 2026-07-18 **headless image**
    - The image no longer contains a shell, busybox or a package
      manager: a compiled `init` binary writes the runtime
      configuration (SQL passdb, TLS, auth policy, spam delivery
      sieve) and starts dovecot directly. All environment knobs
      (`DOVECOT_ALLOW_CLEARTEXT`, `DOVECOT_DEBUG_AUTH`,
      `SPAM_DELIVERY_MODE`, `DEFAULT_PASS_SCHEME`, DB and rspamd
      settings) behave as before.
    - The Bayes autotrainer wrappers `report-spam` / `report-ham` are
      now one static binary execing `rspamc` — same paths, same
      sieve contract as the former shell wrappers.
    - New `init --healthcheck` probe for Docker healthchecks.
