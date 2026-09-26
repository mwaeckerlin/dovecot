# Changelog

- 2026-09-26 **3.2.1**
    - The image builds on arm64 as well as on amd64 and is published for both under one tag, built and published automatically on every change and every week

- 2026-07-20 **env validation + configurable limits + optional quota**
    - Every environment value is whitelist-validated before it is rendered into a config file — a malformed value (a newline above all: config injection) aborts the start with a clear `invalid
      <VAR>` error. Pinned by the new `tests/config-validation.sh`.
    - `SIEVE_MAX_SCRIPT_SIZE` is now an env knob (default 500M) instead of a hardcoded value — lowerable on a small box to shrink the authenticated ManageSieve upload/compile DoS surface.
    - `DOVECOT_QUOTA=yes` (default off) enables a per-mailbox quota read from the PostfixAdmin `mailbox.quota` column (Dovecot 2.4 count backend, sql userdb). Over-quota delivery tempfails (sender retries), never a silent drop. The userdb moved from local.conf into init so it can switch between the static and the sql backend.
    - Project-local tests (npm test): headless image contract and config validation, matching the sibling images' convention.

- 2026-07-18 **headless image**
    - The image no longer contains a shell, busybox or a package manager: a compiled `init` binary writes the runtime configuration (SQL passdb, TLS, auth policy, spam delivery sieve) and starts dovecot directly. All environment knobs (`DOVECOT_ALLOW_CLEARTEXT`, `DOVECOT_DEBUG_AUTH`, `SPAM_DELIVERY_MODE`, `DEFAULT_PASS_SCHEME`, DB and rspamd settings) behave as before.
    - The Bayes autotrainer wrappers `report-spam` / `report-ham` are now one static binary execing `rspamc` — same paths, same sieve contract as the former shell wrappers.
    - New `init --healthcheck` probe for Docker healthchecks.
