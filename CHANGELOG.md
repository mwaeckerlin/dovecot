# Changelog

- 2026-10-30 **3.5.6**
    - The test of the admin sieve block measures the default case with a check that can pass: it asserted with the helper for «this block holds no such line», which demands that the block exists, while without `SIEVE_ADMIN_SCRIPT` it must not exist at all.

- 2026-10-30 **3.5.5**
    - Sieve filters take effect again. The home of a mail user held his scripts in the root of his maildir, so dovecot took the active script `.dovecot.sieve` for a mailbox, failed to open it and aborted every delivery a rule touched. The message then reached INBOX through the implicit keep and no rule of the script took effect, which looks exactly like one single filter being broken. The home now lies under `/var/mail/domains/.sieve/<domain>/<localpart>`, inside the same volume and outside the mailbox namespace, and the start moves what an older image left in the maildir root, so no user loses a filter.
    - A webmail that showed a folder named `dovecot` with a subfolder `sieve` loses it with the same change: that entry was the script file.
    - `SIEVE_ADMIN_SCRIPT` holds a Sieve script of the operator that runs before every user script and that the webmail cannot overwrite. It may route and file, never `reject`: a reject at delivery answers the envelope sender, which unwanted mail forges, so a rule that refuses belongs in postfix. A script that does not compile stops the start, because a filter that silently does nothing is the defect this mechanism exists to avoid.

- 2026-10-23 **3.5.4**
    - The weekly run also rebuilds the version lines 1 and 2, `mwaeckerlin/dovecot:1` and `:2`, from their branches, so they get the security fixes of their base images like the line 3; each line is tested before it is published

- 2026-10-23 **3.5.3**
    - The version line 3 is published as `mwaeckerlin/dovecot:3` from the branch `3`, on every push to it and every week from the run on master, so it gets the security fixes of its base images; it is tested before it is published

- 2026-10-22 **3.5.2**
    - `DB_PORT` sets the port of the PostfixAdmin database; unset, the MySQL default port stays
    - `DB_SSL=no` lets dovecot connect to a database without TLS, such as MariaDB 10, which before needed a change in the init source; unset, nothing changes
    - `DOVECOT_MAX_USERIP_CONNECTIONS` (default `1000`) limits the simultaneous connections of one user from one IP address, `DOVECOT_SIEVE_MAX_USERIP_CONNECTIONS` (default `100`) the same for ManageSieve
    - A test starts the image and reads the configuration dovecot parsed for every new variable, unset and set

- 2026-09-26 **3.5.1**
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
