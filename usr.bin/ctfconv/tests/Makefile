PACKAGE=	tests

ATF_TESTS_SH=	ctfconv_test

# The objects the tests convert are built here, so that they match the
# target and the toolchain of the build, and installed next to the test.
FIXTURES=	a.o a2.o a3.o a5.o az.o b.o ab.o c.o d.o e.o f.o

# GCC emits shapes clang does not (see g.c), and GCC is not in base, so
# these two were built once and are checked in; ctfconv reads any ELF
# class and byte order, so they serve on every target.  Built on amd64
# by gcc14 (FreeBSD Ports Collection) 14.2.0, in the directory of g.c:
#   gcc14 -g -O0 -fno-common -gdwarf-4 -fdebug-prefix-map=$PWD=. \
#       -c -o g-gcc.o g.c
# and the same with -m32 for g-gcc32.o.  The gcc_empty_bound test
# patches bytes of g-gcc32.o at fixed offsets, so a rebuild needs the
# offsets in ctfconv_test.sh rechecked.
GCCFIXTURES=	g-gcc.o g-gcc32.o
${PACKAGE}FILES+=	${GCCFIXTURES}

FXCFLAGS=	-g -O0 -fno-common

a.o: a.c
	${CC} ${FXCFLAGS} -gdwarf-4 -c -o ${.TARGET} ${.ALLSRC}
a2.o: a.c
	${CC} ${FXCFLAGS} -gdwarf-2 -c -o ${.TARGET} ${.ALLSRC}
a3.o: a.c
	${CC} ${FXCFLAGS} -gdwarf-3 -c -o ${.TARGET} ${.ALLSRC}
a5.o: a.c
	${CC} ${FXCFLAGS} -gdwarf-5 -c -o ${.TARGET} ${.ALLSRC}
az.o: a.c
	${CC} ${FXCFLAGS} -gdwarf-4 -gz=zlib -c -o ${.TARGET} ${.ALLSRC}
b.o: b.c
	${CC} ${FXCFLAGS} -gdwarf-4 -c -o ${.TARGET} ${.ALLSRC}
ab.o: a.o b.o
	${LD} -r -o ${.TARGET} ${.ALLSRC}
c.o: c.c
	${CC} ${FXCFLAGS} -gdwarf-4 -c -o ${.TARGET} ${.ALLSRC}
d.o: d.c
	${CC} -g -O2 -fno-common -gdwarf-4 -c -o ${.TARGET} ${.ALLSRC}
# Assembler only: a compilation unit that is not C.
e.o: e.S
	${CC} -g -c -o ${.TARGET} ${.ALLSRC}
# No debug information at all.
f.o: f.c
	${CC} -O0 -c -o ${.TARGET} ${.ALLSRC}
# ThinLTO: the units refer to each other with DW_FORM_ref_addr.  Only
# clang has it; the test skips without the object.
.include <bsd.compiler.mk>
.if ${COMPILER_TYPE} == "clang"
FIXTURES+=	lto.o
lto.o: l1.c l2.c
	${CC} -O2 -g -gdwarf-4 -flto=thin -c ${.ALLSRC}
	${CC} -flto=thin -nostdlib -Wl,-r -o ${.TARGET} l1.o l2.o
CLEANFILES+=	l1.o l2.o
.endif

${PACKAGE}FILES+=	${FIXTURES}
CLEANFILES+=	${FIXTURES}

.include <bsd.test.mk>
