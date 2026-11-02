# FreeBSD build glue for ctfconv (ISC, derived from OpenBSD usr.bin/ctfconv).
# See DONOR-REVISIONS.txt for the imported revisions.

PROG=		ctfconv
SRCS=		ctfconv.c parse.c elf.c dw.c generate.c hash.c xmalloc.c pool.c
MAN=		ctfconv.1

# The CTF format comes from <sys/ctf.h> (ISC, OpenBSD-derived) and the
# DWARF constants from libdwarf's <dwarf.h> (BSD).
LIBADD=		elf z

WARNS?=		6

.include <bsd.prog.mk>
