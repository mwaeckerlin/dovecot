FROM mwaeckerlin/very-base as build
# rspamd-client brings /usr/bin/rspamc — used by learn-spam.sieve /
# learn-ham.sieve via sieve_pipe to teach the Bayes classifier when the
# user moves mail into/out of the Junk folder in their IMAP client.
RUN $PKG_INSTALL dovecot dovecot-mysql dovecot-lmtpd dovecot-pop3d \
        dovecot-pigeonhole-plugin rspamd-client
RUN addgroup -g 5000 login-user
RUN adduser -H -D -u 5000 -G login-user login-user
RUN mkdir -p /var/mail/domains /etc/dovecot/sieve
RUN chown login-user:login-user /var/mail/domains
ADD dovecot.conf /etc/dovecot/dovecot.conf
ADD local.conf /etc/dovecot/local.conf
ADD sieve/learn-spam.sieve /etc/dovecot/sieve/learn-spam.sieve
ADD sieve/learn-ham.sieve /etc/dovecot/sieve/learn-ham.sieve
ADD sieve/report-spam.sh /usr/local/bin/report-spam
ADD sieve/report-ham.sh /usr/local/bin/report-ham
RUN chmod 755 /usr/local/bin/report-spam /usr/local/bin/report-ham
ADD start.sh /start.sh
RUN ${PKG_REMOVE} apk-tools

FROM mwaeckerlin/scratch
COPY --from=build / /
ENV CONTAINERNAME "postfix"
ENV DB_USER       ""
ENV DB_PASSWORD   ""
ENV DB_HOST       ""
ENV DB_NAME       ""
ENV HOSTNAME      ""
ENV DOMAIN        ""
ENV LOCAL_DOMAINS ""
# Rspamd host:port for the Bayes autotrainer — dovecot pipes mail here
# via `rspamc -h <RSPAMD_HOST>:<RSPAMD_CTL_PORT> learn_spam/learn_ham`
# whenever a user moves a message to / out of the Junk folder in IMAP.
ENV RSPAMD_HOST      "rspamd"
ENV RSPAMD_CTL_PORT  "11334"
EXPOSE 143
EXPOSE 993
USER root
CMD /start.sh
VOLUME /var/mail/domains