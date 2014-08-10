# $FreeBSD hyperv-ic Makefile  $

.include <bsd.own.mk>

HV_KVP_DAEMON_DISTDIR?= ${.CURDIR}/../../../contrib/hyperv/tools
.PATH: ${HV_KVP_DAEMON_DISTDIR} 
BINDIR =  /usr/sbin

PROG= 	hv_kvp_daemon
MAN= 	hv_kvp_daemon.8	

${PROG}: ${PROG}.c
	${CC} -o ${PROG}  ${HV_KVP_DAEMON_DISTDIR}/${PROG}.c 

beforeinstall:
	chmod 755 ${HV_KVP_DAEMON_DISTDIR}/kvp-install
	${SH} ${HV_KVP_DAEMON_DISTDIR}/kvp-install PRE-INSTALL	
	
afterinstall:
	${SH} ${HV_KVP_DAEMON_DISTDIR}/kvp-install POST-INSTALL	
	${INSTALL} ${COPY} ${HV_KVP_DAEMON_DISTDIR}/scripts/* /var/db/hyperv/scripts/
	${INSTALL} ${COPY} ${HV_KVP_DAEMON_DISTDIR}/hv_kvpd /etc/rc.d/

.include <bsd.prog.mk>
