/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1987, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

static void usage(void) __dead2;

int
main(int argc, char *argv[])
{
	const char *filename;
	wchar_t *p, *t;
	FILE *fp;
	size_t len;
	int ch, rval;
	wchar_t sep;

	setlocale(LC_ALL, "");

	sep = '\n';
	while ((ch = getopt(argc, argv, "0")) != -1)
		switch(ch) {
		case '0':
			sep = '\0';
			break;
		case '?':
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	fp = stdin;
	filename = "stdin";
	rval = 0;
	do {
		if (*argv) {
			if ((fp = fopen(*argv, "r")) == NULL) {
				warn("%s", *argv);
				rval = 1;
				++argv;
				continue;
			}
			filename = *argv++;
		}
		if (sep == '\n') {
			while ((p = fgetwln(fp, &len)) != NULL) {
				if (p[len - 1] == '\n')
					--len;
				for (t = p + len - 1; t >= p; --t)
					putwchar(*t);
				putwchar('\n');
			}
		} else {
			wchar_t *buf = NULL;
			size_t bufsize = 0;
			wint_t wc;

			len = 0;
			while ((wc = fgetwc(fp)) != WEOF) {
				if (wc == sep) {
					if (len > 0) {
						for (t = buf + len - 1; t >= buf; --t)
							putwchar(*t);
					}
					putwchar(sep);
					len = 0;
				} else {
					if (len >= bufsize) {
						bufsize = bufsize ? bufsize * 2 : 1024;
						buf = reallocarray(buf, bufsize, sizeof(wchar_t));
						if (buf == NULL)
							err(1, NULL);
					}
					buf[len++] = wc;
				}
			}
			if (len > 0) {
				for (t = buf + len - 1; t >= buf; --t)
					putwchar(*t);
				putwchar(sep);
			}
			free(buf);
		}
		if (ferror(fp)) {
			warn("%s", filename);
			clearerr(fp);
			rval = 1;
		}
		(void)fclose(fp);
	} while(*argv);
	exit(rval);
}

void
usage(void)
{
	(void)fprintf(stderr, "usage: rev [-0] [file ...]\n");
	exit(1);
}
