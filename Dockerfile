FROM mwaeckerlin/very-base as build
RUN $PKG_INSTALL dovecot dovecot-mysql dovecot-lmtpd dovecot-pigeonhole-plugin
RUN addgroup -g 5000 login-user
RUN adduser -H -D -u 5000 -G login-user login-user
RUN mkdir -p /var/mail/domains
RUN chown login-user.login-user /var/mail/domains
ADD local.conf /etc/dovecot/local.conf
ADD start.sh /start.sh

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
EXPOSE 143
EXPOSE 993
USER root
CMD /start.sh
VOLUME /var/mail/domains