/*	$OpenBSD: ctfconv.c,v 1.20 2022/10/02 11:56:43 mpi Exp $ */

/*
 * Copyright (c) 2016-2017 Martin Pieuchot
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/capsicum.h>
#include <sys/ctf.h>

#include <assert.h>
#include <capsicum_helpers.h>
#include <elf.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "itype.h"
#include "xmalloc.h"
#include "elf.h"
#include "dw.h"

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

__dead2 void	 usage(void);
int		 convert(const char *, int);
static int	 open_dir_of(const char *, const char **);
static void	 sandbox(int, int);
void		 elf_sort(void);
char		*guess_static_local_name(char *);
struct itype	*find_symb(struct itype *, size_t);
void		 dump_type(struct itype *);
void		 dump_func(struct itype *, int *);
void		 dump_obj(struct itype *, int *);

static struct elfobj		 eo;
const char			*dstrbuf;
size_t				 dstrlen;
static const char		*strtab;
static const struct elfsym	*symtab;
static size_t			 strtabsz, nsymb;

/* Target properties, set from the ELF header. */
int		 target_bigendian;
int		 target_reloc;
unsigned int	 target_psz = sizeof(void *);

const char	*ctf_enc2name(unsigned short);

/* lists of parsed types and functions */
struct itype_queue itypeq = TAILQ_HEAD_INITIALIZER(itypeq);
struct itype_queue ifuncq = TAILQ_HEAD_INITIALIZER(ifuncq);
struct itype_queue iobjq = TAILQ_HEAD_INITIALIZER(iobjq);

__dead2 void
usage(void)
{
	fprintf(stderr, "usage: %s [-l label] [-o outfile] file\n"
	    "       %s -d file\n", getprogname(), getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *filename, *label = NULL, *outfile = NULL, *base = NULL;
	int dump = 0;
	int ch, error = 0, fd, dfd = -1;
	struct itype *it;

	setlocale(LC_ALL, "");

	while ((ch = getopt(argc, argv, "dl:o:")) != -1) {
		switch (ch) {
		case 'd':
			dump = 1;	/* ctfdump(1)-like SUNW_ctf sections */
			break;
		case 'l':
			if (label != NULL)
				usage();
			label = optarg;
			break;
		case 'o':
			if (outfile != NULL)
				usage();
			outfile = optarg;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	/* Either dump the sections, or write it out. */
	if (dump && (outfile != NULL || label != NULL))
		usage();

	filename = *argv;

	/*
	 * Open everything the tool will touch, then enter capability mode
	 * before a byte of the input is interpreted: the
	 * input, and the directory that receives the output, where
	 * in-place mode makes its temporary (3.1) and raw mode its file
	 * (3.2).  Nothing from here on names a path.
	 */
	if ((fd = open(filename, O_RDONLY | O_CLOEXEC)) == -1) {
		warn("open %s", filename);
		return 1;
	}
	if (!dump) {
		dfd = open_dir_of(outfile != NULL ? outfile : filename, &base);
		if (dfd == -1)
			return 1;
	}
	sandbox(fd, dfd);

	error = convert(filename, fd);
	if (error == ELF_NODWARF)
		return 0;	/* no DWARF: nothing to write */
	if (error != 0)
		return 1;

	if (outfile != NULL) {
		/* Raw mode: the container alone. */
		if (generate(dfd, base, outfile, label, 1) != 0)
			return 1;
	} else if (!dump) {
		char	*ctfdata;
		size_t	 ctflen;

		/* In-place mode: a .SUNW_ctf section in the input. */
		if (generate_data(label, 1, &ctfdata, &ctflen) != 0)
			return 1;
		error = elfobj_write_ctf(&eo, dfd, base, ctfdata, ctflen);
		free(ctfdata);
		if (error != 0)
			return 1;
	}

	elfobj_close(&eo);

	if (dump) {
		int fidx = -1, oidx = -1;

		TAILQ_FOREACH(it, &iobjq, it_symb)
			dump_obj(it, &oidx);
		printf("\n");

		TAILQ_FOREACH(it, &ifuncq, it_symb)
			dump_func(it, &fidx);
		printf("\n");

		TAILQ_FOREACH(it, &itypeq, it_next) {
			if (it->it_flags & (ITF_FUNC|ITF_OBJ))
				continue;

			dump_type(it);
		}

		return 0;
	}

	return 0;
}

/*
 * Open the directory holding ``path'' and return the name of the file
 * within it; -1 (reported) if the directory cannot be opened.
 */
static int
open_dir_of(const char *path, const char **basep)
{
	char dir[PATH_MAX];
	const char *slash;
	int dfd;

	slash = strrchr(path, '/');
	if (slash == NULL) {
		strlcpy(dir, ".", sizeof(dir));
		*basep = path;
	} else {
		/* ``/x'' lives in the root. */
		snprintf(dir, sizeof(dir), "%.*s",
		    (int)(slash == path ? 1 : slash - path), path);
		*basep = slash + 1;
	}
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd == -1)
		warn("open %s", dir);
	return dfd;
}

/*
 * Enter capability mode with the descriptors limited to what the modes
 * need: the input is read and mapped (libelf) and read again by the
 * in-place writer; in the output directory a file is created, written,
 * fchmod'ed and fsync'ed, renamed over the input, or unlinked on error.
 * Without a directory (-d) only the input and stdio remain.
 */
static void
sandbox(int fd, int dfd)
{
	cap_rights_t rights;

	cap_rights_init(&rights, CAP_READ, CAP_SEEK, CAP_FSTAT, CAP_MMAP_R);
	if (caph_rights_limit(fd, &rights) == -1)
		err(1, "cap_rights_limit");
	if (dfd != -1) {
		cap_rights_init(&rights, CAP_LOOKUP, CAP_CREATE, CAP_READ,
		    CAP_WRITE, CAP_SEEK, CAP_FTRUNCATE, CAP_FCHMOD, CAP_FSTAT,
		    CAP_FSYNC, CAP_RENAMEAT_SOURCE, CAP_RENAMEAT_TARGET,
		    CAP_UNLINKAT);
		if (caph_rights_limit(dfd, &rights) == -1)
			err(1, "cap_rights_limit");
	}
	if (caph_limit_stdio() == -1)
		err(1, "cap_rights_limit");
	caph_cache_catpages();
	if (caph_enter() == -1)
		err(1, "cap_enter");
}

/*
 * Returns 0 on success, ELF_NODWARF if there is nothing to convert, -1
 * on error.  ``fd'' is the input, open for reading.
 */
int
convert(const char *path, int fd)
{
	int error;

	error = elfobj_open(&eo, path, fd);
	if (error != 0)
		return error;

	/* Target properties: the container follows the file's byte order. */
	target_bigendian = eo.bigendian;
	dw_bigendian = eo.bigendian;
	target_psz = (eo.class == ELFCLASS64) ? 8 : 4;
	target_reloc = (eo.type == ET_REL);

	symtab = eo.syms;
	nsymb = eo.nsyms;
	strtab = eo.strtab;
	strtabsz = eo.strtabsz;
	dstrbuf = eo.str;
	dstrlen = eo.strlen;

	dwarf_parse(eo.info, eo.infolen, eo.abbrev, eo.abbrevlen);

	/* Sort functions */
	elf_sort();

	return 0;
}

/*
 * Guess which part of a local symbol name correspond to the variable
 * name.
 *
 * gcc 4.2.1 emits:
 *
 *	varname.id
 *
 * clang 8 emits:
 *
 *	funcname.varname
 *
 */
char *
guess_static_local_name(char *sname)
{
	const char *errstr;
	char *first, *second;

	first = strtok(sname, ".");
	if (first == NULL)
		return NULL;

	/* Skip meta symbols - gcc style. */
	if (strncmp(first, "__func__", sizeof("__func__") - 1) == 0 ||
	    strncmp(first, "__FUNCTION__", sizeof("__FUNCTION__") - 1) == 0 ||
	    strncmp(first, "__warned", sizeof("__warned") - 1) == 0)
	    	return NULL;

	second = strtok(NULL, "\0");
	if (second == NULL)
		return first;

	/* Skip meta symbols - clang style. */
	if (strncmp(second, "__warned", sizeof("__warned") - 1) == 0)
	    	return NULL;

	/* If `second' isn't a number, assume clang-style name. */
	if (strtonum(second, 1, INT_MAX, &errstr) == 0)
		return second;

	return first;
}

struct itype *
find_symb(struct itype *tmp, size_t stroff)
{
	struct itype		*it;
	char 			*sname, *p;

	if (strtab == NULL || stroff >= strtabsz)
		return NULL;

	sname = xstrdup(strtab + stroff);
	if ((p = guess_static_local_name(sname)) == NULL) {
		free(sname);
		return NULL;
	}

	strlcpy(tmp->it_name, p, ITNAME_MAX);
	free(sname);
	it = RB_FIND(isymb_tree, &isymbt, tmp);

	/* Restore original name */
	if (it == NULL)
		strlcpy(tmp->it_name, (strtab + stroff), ITNAME_MAX);

	return it;
}

void
elf_sort(void)
{
	struct itype		*it, tmp;
	size_t			 i;

	memset(&tmp, 0, sizeof(tmp));
	for (i = 0; i < nsymb; i++) {
		const struct elfsym *st = &symtab[i];

		if (st->st_shndx == SHN_UNDEF || st->st_shndx == SHN_COMMON)
			continue;

		switch (ELF_ST_TYPE(st->st_info)) {
		case STT_FUNC:
			tmp.it_flags = ITF_FUNC;
			break;
		case STT_OBJECT:
			tmp.it_flags = ITF_OBJ;
			break;
		default:
			continue;
		}

		it = find_symb(&tmp, st->st_name);
		if (it == NULL) {
			/* Insert 'unknown' entry to match symbol order. */
			it = it_dup(&tmp);
			it->it_refp = it;
#ifdef DEBUG
			warnx("symbol not found: %s", it_name(it));
#endif
		}

		if (it->it_flags & ITF_INSERTED) {
#ifdef DEBUG
			warnx("%s: already inserted", it_name(it));
#endif
			it = it_dup(it);
		}

		/* Save symbol index for dump. */
		it->it_ref = i;

		it->it_flags |= ITF_INSERTED;
		if (it->it_flags & ITF_FUNC)
			TAILQ_INSERT_TAIL(&ifuncq, it, it_symb);
		else
			TAILQ_INSERT_TAIL(&iobjq, it, it_symb);
	}
}

static const char *
type_name(struct itype *it)
{
	const char *name;

	name = it_name(it);
	if (name == NULL)
		return "(anon)";

	return name;
}

/* Display parsed types a la ctfdump(1) */
void
dump_type(struct itype *it)
{
	struct imember *im;

#ifdef DEBUG
	switch (it->it_type) {
	case CTF_K_POINTER:
	case CTF_K_TYPEDEF:
	case CTF_K_VOLATILE:
	case CTF_K_CONST:
	case CTF_K_RESTRICT:
	case CTF_K_ARRAY:
	case CTF_K_FUNCTION:
		if (it->it_refp == NULL) {
			printf("unresolved: %s type=%d\n", it_name(it),
			    it->it_type);
			return;
		}
	default:
		break;
	}
#endif

	switch (it->it_type) {
	case CTF_K_FLOAT:
	case CTF_K_INTEGER:
		printf("  [%u] %s %s encoding=%s offset=0 bits=%u\n",
		    it->it_idx,
		    (it->it_type == CTF_K_INTEGER) ? "INTEGER" : "FLOAT",
		    it_name(it), ctf_enc2name(it->it_enc), it->it_size);
		break;
	case CTF_K_POINTER:
		printf("  <%u> POINTER %s refers to %u\n", it->it_idx,
		    type_name(it), it->it_refp->it_idx);
		break;
	case CTF_K_TYPEDEF:
		printf("  <%u> TYPEDEF %s refers to %u\n",
		    it->it_idx, it_name(it), it->it_refp->it_idx);
		break;
	case CTF_K_VOLATILE:
		printf("  <%u> VOLATILE %s refers to %u\n", it->it_idx,
		    type_name(it), it->it_refp->it_idx);
		break;
	case CTF_K_CONST:
		printf("  <%u> CONST %s refers to %u\n", it->it_idx,
		    type_name(it), it->it_refp->it_idx);
		break;
	case CTF_K_RESTRICT:
		printf("  <%u> RESTRICT %s refers to %u\n", it->it_idx,
		    it_name(it), it->it_refp->it_idx);
		break;
	case CTF_K_ARRAY:
		printf("  [%u] ARRAY %s content: %u index: %u nelems: %u\n",
		    it->it_idx, type_name(it), it->it_refp->it_idx, long_tidx,
		    it->it_nelems);
		printf("\n");
		break;
	case CTF_K_STRUCT:
	case CTF_K_UNION:
		printf("  [%u] %s %s (%u bytes)\n", it->it_idx,
		    (it->it_type == CTF_K_STRUCT) ? "STRUCT" : "UNION",
		    type_name(it), it->it_size);
		TAILQ_FOREACH(im, &it->it_members, im_next) {
			printf("\t%s type=%u off=%zu\n",
			    (im_name(im) == NULL) ? "unknown" : im_name(im),
			    im->im_refp ? im->im_refp->it_idx : 0, im->im_off);
		}
		printf("\n");
		break;
	case CTF_K_ENUM:
		printf("  [%u] ENUM %s\n", it->it_idx, type_name(it));
		TAILQ_FOREACH(im, &it->it_members, im_next) {
			printf("\t%s = %zu\n", im_name(im), im->im_ref);
		}
		printf("\n");
		break;
	case CTF_K_FUNCTION:
		printf("  [%u] FUNCTION (%s) returns: %u args: (",
		    it->it_idx, (it_name(it) != NULL) ? it_name(it) : "anon",
		    it->it_refp->it_idx);
		TAILQ_FOREACH(im, &it->it_members, im_next) {
			printf("%u%s", im->im_refp->it_idx,
			    TAILQ_NEXT(im, im_next) ? ", " : "");
		}
		printf(")\n");
		break;
	default:
		assert(0 == 1);
	}
}

void
dump_func(struct itype *it, int *idx)
{
	struct imember *im;

	(*idx)++;

	if (it->it_type == CTF_K_UNKNOWN && it->it_nelems == 0)
		return;

	printf("  [%u] FUNC (%s) returns: %u args: (", (*idx),
	    (it_name(it) != NULL) ? it_name(it) : "unknown",
	    it->it_refp->it_idx);
	TAILQ_FOREACH(im, &it->it_members, im_next) {
		printf("%u%s", im->im_refp->it_idx,
		    TAILQ_NEXT(im, im_next) ? ", " : "");
	}
	printf(")\n");
}

void
dump_obj(struct itype *it, int *idx)
{
	int l;

	(*idx)++;

	l = printf("  [%u] %u", (*idx), it->it_refp->it_idx);
	printf("%*s %s (%" PRIu64 ")\n", 14 - l, "", it_name(it), it->it_ref);
}

const char *
ctf_enc2name(unsigned short enc)
{
	static const char *enc_name[] = { "SIGNED", "CHAR", "SIGNED CHAR",
	    "BOOL", "SIGNED BOOL" };
	static char invalid[7];

	if (enc == CTF_INT_VARARGS)
		return "VARARGS";

	if (enc > 0 && enc < nitems(enc_name))
		return enc_name[enc - 1];

	snprintf(invalid, sizeof(invalid), "0x%x", enc);
	return invalid;
}
