FROM mwaeckerlin/very-base AS build
# rspamd-client brings /usr/bin/rspamc — used by learn-spam.sieve /
# learn-ham.sieve via sieve_pipe to teach the Bayes classifier when the
# user moves mail into/out of the Junk folder in their IMAP client.
RUN $PKG_INSTALL dovecot dovecot-mysql dovecot-lmtpd dovecot-pop3d \
        dovecot-pigeonhole-plugin rspamd-client
RUN addgroup -g 5000 login-user
RUN adduser -H -D -u 5000 -G login-user login-user
RUN mkdir -p /var/mail/domains /etc/dovecot/sieve
RUN chown login-user:login-user /var/mail/domains
COPY dovecot.conf /etc/dovecot/dovecot.conf
COPY local.conf /etc/dovecot/local.conf
COPY sieve/learn-spam.sieve /etc/dovecot/sieve/learn-spam.sieve
COPY sieve/learn-ham.sieve /etc/dovecot/sieve/learn-ham.sieve
COPY sieve/report-spam.sh /usr/local/bin/report-spam
COPY sieve/report-ham.sh /usr/local/bin/report-ham
RUN chmod 755 /usr/local/bin/report-spam /usr/local/bin/report-ham
# Pre-compile the global sieve scripts: /etc/dovecot/sieve stays
# root-owned read-only, and the mail user (uid 5000) that executes
# the scripts at IMAP time has no write access to store the compiled
# .svbin next to the source — without the precompiled binary every
# trigger fails with «Permission denied».
RUN sievec /etc/dovecot/sieve/learn-spam.sieve
RUN sievec /etc/dovecot/sieve/learn-ham.sieve
COPY start.sh /start.sh
RUN ${PKG_REMOVE} apk-tools

FROM mwaeckerlin/scratch
COPY --from=build / /
ENV DB_USER=""
ENV DB_PASSWORD=""
ENV DB_HOST=""
ENV DB_NAME=""
ENV DOMAIN=""
ENV DEFAULT_PASS_SCHEME="SHA512-CRYPT"
# Rspamd host:port for the Bayes autotrainer — dovecot pipes mail here
# via `rspamc -h <RSPAMD_HOST>:<RSPAMD_CTL_PORT> learn_spam/learn_ham`
# whenever a user moves a message to / out of the Junk folder in IMAP.
ENV RSPAMD_HOST="rspamd"
ENV RSPAMD_CTL_PORT="11334"
# Force auth over TLS by default. Set to "yes" only if you deliberately
# want to allow cleartext logins on unencrypted connections.
ENV DOVECOT_ALLOW_CLEARTEXT="no"
# Verbose auth debug logging is never a production default — set to
# "yes" only as a deliberate diagnostics override (see README).
ENV DOVECOT_DEBUG_AUTH="no"
# Client-facing ports: POP3, IMAP, IMAPS, POP3S, ManageSieve. The LMTP
# (24) and SASL (12345) listeners are internal-only — reachable for
# postfix on the shared docker network, never to be published.
EXPOSE 110
EXPOSE 143
EXPOSE 993
EXPOSE 995
EXPOSE 4190
# Trade-off: the dovecot master process must run as root to bind the
# service listeners and to spawn per-login worker processes; every
# IMAP/POP3 session, sieve execution and mail delivery then runs as
# the unprivileged mail user (uid/gid 5000, see userdb in local.conf).
USER root
CMD /start.sh
VOLUME /var/mail/domains
