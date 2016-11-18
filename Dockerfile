FROM ubuntu
MAINTAINER mwaeckerlin
ENV TERM xterm

ENV LDAP_HOST ""
ENV LDAP_DOMAIN ""
ENV LDAP_BASE ""
ENV LDAP_URL ""
ENV LDAP_URL_QUERY "?uid?sub?(objectClass=posixAccount)"
ENV LDAP_BIND_DN ""
ENV LDAP_BIND_PWD ""

EXPOSE 143
EXPOSE 993

RUN apt-get update
RUN apt-get install -y dovecot-core dovecot-imapd dovecot-ldap dovecot-managesieved dovecot-sieve dovecot-lmtpd

ADD local.conf /etc/dovecot/local.conf
ADD dovecot-ldap.conf /etc/dovecot/dovecot-ldap.conf
ADD start.sh /start.sh
CMD /start.sh
