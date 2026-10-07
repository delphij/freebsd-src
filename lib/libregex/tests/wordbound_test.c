/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Word boundaries next to characters of more than one byte.  A fullwidth
 * A (U+FF21, three bytes in UTF-8) and a sigma (U+03C3, two bytes) are
 * letters; '|' is not.
 */

#include <locale.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atf-c.h>

/*
 * This file is also built as asan_test, with the regex sources compiled
 * in under the address sanitizer and their entry points renamed on the
 * compiler's command line, in this file as in theirs.
 */

#define	FWA	"\357\274\241"		/* U+FF21 */
#define	SIGMA	"\317\203"		/* U+03C3 */

static regmatch_t sub;		/* what the first subexpression matched */

/*
 * Compile pat as a basic expression and run it over str with REG_STARTEND,
 * as grep(1) does, asking for the match and for one subexpression, so
 * that the matcher has to find out where everything is.  Return the
 * result, and the match in so and eo.
 */
static int
run(const char *pat, const char *str, regoff_t *so, regoff_t *eo)
{
	regex_t re;
	regmatch_t pm[2];
	int r;

	ATF_REQUIRE_EQ(0, regcomp(&re, pat, REG_NEWLINE));
	pm[0].rm_so = 0;
	pm[0].rm_eo = strlen(str);
	pm[1].rm_so = pm[1].rm_eo = -1;
	r = regexec(&re, str, 2, pm, REG_STARTEND);
	regfree(&re);
	*so = r == 0 ? pm[0].rm_so : -1;
	*eo = r == 0 ? pm[0].rm_eo : -1;
	sub = pm[1];
	return (r);
}

/*
 * The same, with the search starting at offset so of str and the start
 * not taken for the beginning of a line, as grep(1) does to go on after
 * a match.
 */
static int
runfrom(const char *pat, const char *str, regoff_t so, regoff_t *mso,
    regoff_t *meo)
{
	regex_t re;
	regmatch_t pm[2];
	int r;

	ATF_REQUIRE_EQ(0, regcomp(&re, pat, REG_NEWLINE));
	pm[0].rm_so = so;
	pm[0].rm_eo = strlen(str);
	r = regexec(&re, str, 2, pm, REG_STARTEND | REG_NOTBOL);
	regfree(&re);
	*mso = r == 0 ? pm[0].rm_so : -1;
	*meo = r == 0 ? pm[0].rm_eo : -1;
	return (r);
}

#define	MATCH(pat, str, s, e) do {				\
	regoff_t so, eo;						\
	ATF_CHECK_EQ_MSG(0, run(pat, str, &so, &eo),			\
	    "%s on %s", pat, str);					\
	ATF_CHECK_EQ_MSG(s, so, "%s on %s: starts at %ld", pat, str,	\
	    (long)so);							\
	ATF_CHECK_EQ_MSG(e, eo, "%s on %s: ends at %ld", pat, str,	\
	    (long)eo);							\
} while (0)
/* A match, and what its first subexpression matched. */
#define	MATCHSUB(pat, str, s, e, s1, e1) do {			\
	MATCH(pat, str, s, e);						\
	ATF_CHECK_EQ_MSG(s1, sub.rm_so, "%s on %s: group at %ld",	\
	    pat, str, (long)sub.rm_so);					\
	ATF_CHECK_EQ_MSG(e1, sub.rm_eo, "%s on %s: group to %ld",	\
	    pat, str, (long)sub.rm_eo);					\
} while (0)
#define	NOMATCH(pat, str) do {					\
	regoff_t so, eo;						\
	ATF_CHECK_EQ_MSG(REG_NOMATCH, run(pat, str, &so, &eo),		\
	    "%s on %s", pat, str);					\
} while (0)
#define	MATCHFROM(pat, str, from, s, e) do {			\
	regoff_t so, eo;						\
	ATF_CHECK_EQ_MSG(0, runfrom(pat, str, from, &so, &eo),		\
	    "%s on %s from %d", pat, str, from);			\
	ATF_CHECK_EQ_MSG(s, so, "%s on %s from %d: starts at %ld",	\
	    pat, str, from, (long)so);					\
	ATF_CHECK_EQ_MSG(e, eo, "%s on %s from %d: ends at %ld",	\
	    pat, str, from, (long)eo);					\
} while (0)
#define	NOMATCHFROM(pat, str, from) do {				\
	regoff_t so, eo;						\
	ATF_CHECK_EQ_MSG(REG_NOMATCH,					\
	    runfrom(pat, str, from, &so, &eo),				\
	    "%s on %s from %d", pat, str, from);			\
} while (0)

ATF_TC_WITHOUT_HEAD(utf8);
ATF_TC_BODY(utf8, tc)
{

	ATF_REQUIRE(setlocale(LC_CTYPE, "C.UTF-8") != NULL);

	/* Between two letters there is no boundary. */
	NOMATCH("\\b" FWA, SIGMA FWA);
	NOMATCH("\\b" FWA, "a" FWA);
	NOMATCH(FWA "\\b", FWA SIGMA);
	NOMATCH(FWA "\\b", FWA "a");
	NOMATCH("\\<" FWA, SIGMA FWA);
	NOMATCH(SIGMA "\\>", SIGMA FWA);
	MATCH(SIGMA "\\B" FWA, SIGMA FWA, 0, 5);

	/* Between a letter and something else there is one. */
	MATCH("\\b" FWA, "|" FWA, 1, 4);
	MATCH(FWA "\\b", FWA "|", 0, 3);
	MATCH("\\<" FWA, "|" FWA, 1, 4);
	MATCH(FWA "\\>", FWA "|", 0, 3);
	MATCH("\\b" SIGMA, "|" SIGMA, 1, 3);
	NOMATCH("|\\B" SIGMA, "|" SIGMA);

	/* At the ends of the string. */
	MATCH("\\b" FWA, FWA, 0, 3);
	MATCH(FWA "\\b", FWA, 0, 3);
	MATCH("\\<" SIGMA, SIGMA, 0, 2);
	MATCH(SIGMA "\\>", SIGMA, 0, 2);

	/*
	 * The matcher looks for the string the match must contain first
	 * and then walks from there; a boundary before that string is then
	 * judged from the character before it, which has to be decoded
	 * backwards.
	 */
	MATCH("\\b" FWA, "x|" FWA, 2, 5);
	NOMATCH("\\b" FWA, "x" SIGMA FWA);
	MATCH("\\b" FWA, "x" SIGMA "|" FWA, 4, 7);
	MATCH(SIGMA "\\>", "x" SIGMA "|" FWA, 1, 3);

	/*
	 * A match found by a first pass with the boundary judged from the
	 * decoded characters, then looked for again from its start with
	 * the boundary judged from the byte before it, used to drive the
	 * matcher past the end of the string.
	 */
	NOMATCH("\\>" FWA, FWA "|K|" SIGMA SIGMA FWA "|");
	MATCH("\\b" FWA, FWA "|K|" SIGMA SIGMA FWA "|", 0, 3);
	MATCH("\\<" FWA "|", FWA "|K|" SIGMA SIGMA FWA "|", 0, 4);
	MATCH("\\b" FWA "|", "|" FWA "|K|" SIGMA SIGMA FWA "|", 1, 5);
	MATCH(FWA "\\b", "|" FWA "|K|" SIGMA SIGMA FWA "|", 1, 4);
	MATCH("\\>" "|", FWA "|K|" SIGMA SIGMA FWA "|", 3, 4);

	/* A byte that is not part of any character is not part of a word. */
	MATCH("\\bb", FWA "\377b", 4, 5);
	MATCH("\\<b", FWA "\377b", 4, 5);
	MATCH(FWA "\\b", FWA "\377b", 0, 3);
	NOMATCH(FWA "\\B", FWA "\377b");

	/*
	 * A search that starts inside the subject looks at the character
	 * before its start, which is read backwards.
	 */
	MATCHFROM("\\b" FWA, "|" FWA, 1, 1, 4);
	NOMATCHFROM("\\b" FWA, SIGMA FWA, 2);
	NOMATCHFROM("\\b" FWA, "a" FWA, 1);
	MATCHFROM("\\B" FWA, SIGMA FWA, 2, 2, 5);
	MATCHFROM("\\<" FWA, "\377" FWA, 1, 1, 4);
	NOMATCHFROM("\\<" FWA, SIGMA SIGMA FWA, 4);

	/* The subexpressions are placed by a pass of their own. */
	MATCHSUB("\\(" SIGMA "*\\)\\b" FWA, SIGMA SIGMA "|" FWA, 5, 8, 5, 5);
	NOMATCH("\\(" SIGMA "*\\)\\b" FWA, SIGMA SIGMA "|" SIGMA FWA);
	MATCHSUB("\\b\\(" FWA "\\)*\\b", "|" FWA FWA "|", 1, 7, 4, 7);
	MATCHSUB("\\(" FWA "\\|" SIGMA "\\)*\\>", SIGMA FWA "a", 6, 6, -1, -1);
	MATCHSUB("\\(" FWA "\\|" SIGMA "\\)*\\>", SIGMA FWA "|", 0, 5, 2, 5);
	MATCHSUB("\\<\\(" FWA "\\)", "a|" FWA, 2, 5, 2, 5);

	/* With a back-reference, the boundaries are checked another way. */
	MATCHSUB("\\(" FWA "\\)\\1\\b", FWA FWA "|", 0, 6, 0, 3);
	NOMATCH("\\(" FWA "\\)\\1\\b", FWA FWA SIGMA);
	MATCH("\\b" FWA "\\(" SIGMA "\\)\\1", "|" FWA SIGMA SIGMA, 1, 8);
	NOMATCH("\\b" FWA "\\(" SIGMA "\\)\\1", "a" FWA SIGMA SIGMA);
	MATCH("\\(" SIGMA "\\)\\B\\1", SIGMA SIGMA, 0, 4);
	NOMATCH("\\(" SIGMA "\\)\\b\\1", SIGMA SIGMA);
}

/*
 * In SJIS, Big5 and GBK the second byte of a character can be a letter
 * of ASCII: the ideographic comma of SJIS is 0x81 0x41, and 0x41 alone
 * is 'A'.  Read backwards from the 'b', a line could end in "Ab"; the
 * matcher, reading forwards, knows that it does not, and must not
 * disagree with itself about the boundary before the 'b'.
 */
static void
trailing(const char *locale, const char *comma)
{
	char cb[8], cab[8], pat[32];

	if (setlocale(LC_CTYPE, locale) == NULL || MB_CUR_MAX < 2)
		atf_tc_skip("the %s locale is not available", locale);

	snprintf(cb, sizeof(cb), "%sb", comma);
	snprintf(cab, sizeof(cab), "%sAb", comma);

	snprintf(pat, sizeof(pat), "\\(%s\\)*\\bb", comma);
	MATCHSUB(pat, cb, 0, 3, 0, 2);
	snprintf(pat, sizeof(pat), "\\(%s\\)*\\<b", comma);
	MATCHSUB(pat, cb, 0, 3, 0, 2);
	MATCH("\\bb", cb, 2, 3);
	MATCH("\\(b\\)", cb, 2, 3);
	NOMATCH("A\\bb", cb);
	snprintf(pat, sizeof(pat), "%s\\Bb", comma);
	NOMATCH(pat, cb);
	NOMATCH("\\bb", cab);
	MATCH("\\(A\\)b\\b", cab, 2, 4);
	snprintf(pat, sizeof(pat), "\\(%s\\)*\\BA", comma);
	NOMATCH(pat, cab);
}

/*
 * A search that starts inside the subject reads the character before its
 * start backwards, and after a trailing byte that is also a letter of
 * ASCII that reading is wrong: the search cannot know that the 'A' is
 * the second byte of the comma.  Decoding from the start of the string
 * would make every such search cost as much as the string is long.
 */
static void
trailing_from(const char *locale, const char *comma)
{
	char cb[8];

	if (setlocale(LC_CTYPE, locale) == NULL || MB_CUR_MAX < 2)
		atf_tc_skip("the %s locale is not available", locale);
	snprintf(cb, sizeof(cb), "%sb", comma);
	atf_tc_expect_fail("the character before a search start is read "
	    "backwards");
	MATCHFROM("\\bb", cb, 2, 2, 3);
}

ATF_TC_WITHOUT_HEAD(sjis_from);
ATF_TC_BODY(sjis_from, tc)
{

	trailing_from("ja_JP.SJIS", "\201\101");
}

ATF_TC_WITHOUT_HEAD(sjis);
ATF_TC_BODY(sjis, tc)
{

	trailing("ja_JP.SJIS", "\201\101");
}

ATF_TC_WITHOUT_HEAD(big5);
ATF_TC_BODY(big5, tc)
{

	trailing("zh_TW.Big5", "\241\101");
}

ATF_TC_WITHOUT_HEAD(gbk);
ATF_TC_BODY(gbk, tc)
{

	trailing("zh_CN.GBK", "\201\101");
}

/*
 * The EUC decoders take any byte after a lead byte, a newline included.
 * With REG_NEWLINE, the matcher must then see one character and no line
 * start, however it got to the position after it.
 */
ATF_TC_WITHOUT_HEAD(eucjp_newline);
ATF_TC_BODY(eucjp_newline, tc)
{

	if (setlocale(LC_CTYPE, "ja_JP.eucJP") == NULL)
		atf_tc_skip("the ja_JP.eucJP locale is not available");

	MATCH("^bx\\|b", "\241\nbx", 2, 3);
	MATCH("\\(^bx\\)*b", "\241\nbx", 2, 3);
	NOMATCH("^b", "\241\nbx");
	MATCH("^b", "\241\n\nbx", 3, 4);
}

/*
 * Random expressions with boundaries, groups and repetitions over random
 * subjects of letters, non-letters, newlines and invalid bytes, in UTF-8
 * and SJIS.  Asking for no match, the match, or the match and a
 * subexpression must give the same answer, and nothing may fault.
 */
static const char *utf8_alpha[] = {
	SIGMA, FWA, "a", "b", "|", "_", "1", "\n", "\377", "\357\274"
};
static const char *sjis_alpha[] = {
	"\201\101", "\201\134", "\203\101", "\203\134", "A", "b", "|", "_",
	"1", "\n", "\261", "\201"
};
static const char *bounds[] = {
	"\\<", "\\>", "\\b", "\\B", "[[:<:]]", "[[:>:]]"
};

static void
randomly(const char *locale, const char **alpha, int nalpha, int cases)
{
	regex_t re;
	regmatch_t pm0, pm1, pm2[2];
	char pat[200], str[200], atom[40];
	const char *a;
	int i, j, k, n, r0, r1, r2, cf;

	if (setlocale(LC_CTYPE, locale) == NULL)
		atf_tc_skip("the %s locale is not available", locale);
	srandom(20261006);
	for (i = 0; i < cases; i++) {
		str[0] = '\0';
		n = random() % 12;
		for (j = 0; j < n; j++)
			strlcat(str, alpha[random() % nalpha], sizeof(str));
		cf = REG_NEWLINE | (random() % 2 ? REG_EXTENDED : 0);
		pat[0] = '\0';
		n = 1 + random() % 4;
		for (j = 0; j < n; j++) {
			if (random() % 3 == 0)
				strlcat(pat, bounds[random() % 6], sizeof(pat));
			a = alpha[random() % nalpha];
			if (a[0] == '\n' || a[0] == '|' || (a[0] & 0x80) != 0)
				a = "b";
			k = random() % 5;
			if (k == 0)
				snprintf(atom, sizeof(atom), "%s", a);
			else if (k == 1)
				snprintf(atom, sizeof(atom), cf & REG_EXTENDED ?
				    "(%s)" : "\\(%s\\)", a);
			else if (k == 2)
				snprintf(atom, sizeof(atom), cf & REG_EXTENDED ?
				    "(%s)*" : "\\(%s\\)*", a);
			else if (k == 3)
				snprintf(atom, sizeof(atom), "%s*", a);
			else
				snprintf(atom, sizeof(atom), ".");
			strlcat(pat, atom, sizeof(pat));
		}
		if (random() % 3 == 0)
			strlcat(pat, bounds[random() % 6], sizeof(pat));
		if (regcomp(&re, pat, cf) != 0)
			continue;
		pm0.rm_so = pm1.rm_so = pm2[0].rm_so = 0;
		pm0.rm_eo = pm1.rm_eo = pm2[0].rm_eo = strlen(str);
		r0 = regexec(&re, str, 0, &pm0, REG_STARTEND);
		r1 = regexec(&re, str, 1, &pm1, REG_STARTEND);
		r2 = regexec(&re, str, 2, pm2, REG_STARTEND);
		regfree(&re);
		ATF_CHECK_MSG(r0 == r1 && r1 == r2 && (r1 != 0 ||
		    (pm1.rm_so == pm2[0].rm_so && pm1.rm_eo == pm2[0].rm_eo)),
		    "case %d: %s in %s gives %d, %d, %d", i, pat, locale, r0,
		    r1, r2);
	}
}

ATF_TC_WITHOUT_HEAD(random_utf8);
ATF_TC_BODY(random_utf8, tc)
{

	randomly("C.UTF-8", utf8_alpha, 10, 3000);
}

ATF_TC_WITHOUT_HEAD(random_sjis);
ATF_TC_BODY(random_sjis, tc)
{

	randomly("ja_JP.SJIS", sjis_alpha, 12, 3000);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, utf8);
	ATF_TP_ADD_TC(tp, sjis);
	ATF_TP_ADD_TC(tp, sjis_from);
	ATF_TP_ADD_TC(tp, big5);
	ATF_TP_ADD_TC(tp, gbk);
	ATF_TP_ADD_TC(tp, eucjp_newline);
	ATF_TP_ADD_TC(tp, random_utf8);
	ATF_TP_ADD_TC(tp, random_sjis);
	return (atf_no_error());
}
