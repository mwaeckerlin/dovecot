FROM mwaeckerlin/very-base AS init
RUN $PKG_INSTALL g++
COPY init.cpp .
COPY sieve/report.cpp .
RUN g++ -static -Os -flto=auto -fno-rtti -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -Wl,-s -std=c++20 -o init init.cpp
RUN g++ -static -Os -flto=auto -fno-rtti -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -Wl,-s -std=c++20 -o report report.cpp
RUN strip -s -R .comment -R .gnu.version --strip-unneeded init report

FROM mwaeckerlin/very-base AS build
# rspamd-client brings /usr/bin/rspamc — used by learn-spam.sieve /
# learn-ham.sieve via sieve_pipe to teach the Bayes classifier when the
# user moves mail into/out of the Junk folder in their IMAP client.
RUN $PKG_INSTALL dovecot dovecot-mysql dovecot-lmtpd dovecot-pop3d \
        dovecot-pigeonhole-plugin rspamd-client ca-certificates
RUN addgroup -g 5000 login-user
RUN adduser -H -D -u 5000 -G login-user login-user
RUN mkdir -p /var/mail/domains /etc/dovecot/sieve /etc/dovecot/conf.d \
             /run/dovecot /var/lib/dovecot /tmp
RUN chmod 1777 /tmp
RUN chown login-user:login-user /var/mail/domains
COPY dovecot.conf /etc/dovecot/dovecot.conf
COPY local.conf /etc/dovecot/local.conf
COPY sieve/learn-spam.sieve /etc/dovecot/sieve/learn-spam.sieve
COPY sieve/learn-ham.sieve /etc/dovecot/sieve/learn-ham.sieve
# One static binary, two names: the basename picks learn_spam or
# learn_ham (replaces the former report-spam.sh / report-ham.sh shell
# wrappers — the headless image has no shell to run them).
COPY --from=init report /usr/local/bin/report-spam
COPY --from=init report /usr/local/bin/report-ham
RUN chmod 755 /usr/local/bin/report-spam /usr/local/bin/report-ham
# Pre-compile the global sieve scripts: /etc/dovecot/sieve stays
# root-owned read-only, and the mail user (uid 5000) that executes
# the scripts at IMAP time has no write access to store the compiled
# .svbin next to the source — without the precompiled binary every
# trigger fails with «Permission denied».
RUN sievec /etc/dovecot/sieve/learn-spam.sieve
RUN sievec /etc/dovecot/sieve/learn-ham.sieve
COPY --from=init init /usr/bin/init
# Shell scripts shipped by the dovecot package (full-text-search
# helper) — the headless image has no shell, so they must not ship.
RUN rm -f /usr/libexec/dovecot/decode2text.sh

# Collect only the binaries, shared libraries and configs the runtime
# actually needs into /root/ — no shell, no package manager, no
# busybox. musl's `ldd` accepts exactly ONE file per invocation, so
# deps are gathered in a per-file loop (this also pulls libmariadb for
# the SQL passdb); /lib/ld-musl-x86_64.so.1 is the ELF interpreter and
# listed explicitly.
RUN tar cph \
        /etc/dovecot /var/mail/domains /run/dovecot /var/lib/dovecot \
        /etc/passwd /etc/group /etc/services /etc/nsswitch.conf \
        /etc/ssl/certs /etc/ssl/cert.pem /usr/share/ca-certificates \
        /usr/sbin/dovecot /usr/libexec/dovecot /usr/lib/dovecot \
        /usr/share/icu \
        /usr/bin/doveconf /usr/bin/doveadm /usr/bin/sievec \
        /usr/bin/rspamc \
        /usr/local/bin/report-spam /usr/local/bin/report-ham \
        /usr/bin/init /lib/ld-musl-x86_64.so.1 /tmp \
        $(for f in /usr/sbin/dovecot /usr/libexec/dovecot/* \
                   /usr/lib/dovecot/*.so* /usr/lib/dovecot/*/*.so* \
                   /usr/bin/doveconf /usr/bin/doveadm /usr/bin/sievec \
                   /usr/bin/rspamc; do \
              ldd "$f" 2>/dev/null | sed -n 's,.* => \([^ ]*\) .*,\1,p'; \
          done | sort -u) \
    | tar xpC /root/

FROM mwaeckerlin/scratch
ENV CONTAINERNAME="dovecot" \
    DB_USER="" \
    DB_PASSWORD="" \
    DB_HOST="" \
    DB_NAME="" \
    DOMAIN="" \
    DEFAULT_PASS_SCHEME="SHA512-CRYPT" \
    SPAM_DELIVERY_MODE="reject"
# Rspamd host:port for the Bayes autotrainer — dovecot pipes mail here
# via `rspamc -h <RSPAMD_HOST>:<RSPAMD_CTL_PORT> learn_spam/learn_ham`
# whenever a user moves a message to / out of the Junk folder in IMAP.
ENV RSPAMD_HOST="rspamd" \
    RSPAMD_CTL_PORT="11334"
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
ENTRYPOINT ["/usr/bin/init"]
VOLUME /var/mail/domains
COPY --from=build /root/ /
