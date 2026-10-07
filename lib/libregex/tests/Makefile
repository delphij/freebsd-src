PACKAGE=	tests
TESTSRC=	${SRCTOP}/contrib/netbsd-tests/lib/libc/regex

.PATH:		${SRCTOP}/tests

.include "${SRCTOP}/lib/libc/tests/regex/Makefile.inc"

ATF_TESTS_SH+=	libregex_test
ATF_TESTS_C+=	wordbound_test

${PACKAGE}DATA_FILES+=	gnuext.in

LIBADD.h_regex+=regex
LIBADD.wordbound_test+=	regex

# The same cases run against a copy of the regex sources compiled into
# the test under the address sanitizer, which sees what a test of the
# library cannot.  The entry points are renamed so that neither libc's
# nor the sanitizer's own stand in their way.
.if ${COMPILER_TYPE} == "clang" && !defined(_SKIP_BUILD)
.include "${SRCTOP}/lib/libclang_rt/compiler-rt-vars.mk"
_libclang_rt_asan=	${SYSROOT}${SANITIZER_LIBDIR}/libclang_rt.asan-${CRTARCH}.a
.if exists(${_libclang_rt_asan})
ATF_TESTS_C+=	asan_test
REGEXSRC=	${SRCTOP}/lib/libc/regex
.PATH:		${REGEXSRC}
SRCS.asan_test=	asan_test.c regcomp.c regexec.c regerror.c regfree.c
CFLAGS.asan_test+=	-DLIBREGEX -DPOSIX_MISTAKE -I${REGEXSRC} \
			-I${SRCTOP}/lib/libc/locale \
			-Dregcomp=asan_regcomp -Dregexec=asan_regexec \
			-Dregerror=asan_regerror -Dregfree=asan_regfree \
			-fsanitize=address
LDFLAGS.asan_test+=	-fsanitize=address ${SANITIZER_LDFLAGS}
# The library's sources are not held to the tests' warnings.
CFLAGS.regexec.c+=	-Wno-sign-compare
.elif make(all)
.info Could not find runtime library ${_libclang_rt_asan}, skipping asan_test
.endif
.endif
.for t in ${NETBSD_ATF_TESTS_C}
LIBADD.$t+=	regex
.endfor

.include <netbsd-tests.test.mk>
.include <bsd.test.mk>
