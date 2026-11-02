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
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/capsicum.h>
#include <sys/ctf.h>

#include <assert.h>
#include <capsicum_helpers.h>
#include <elf.h>
#include <err.h>
#include <errno.h>
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
void		 strip_symbol_suffix(char *);
struct itype	*find_symb_named(struct itype *, const char *, const char *);
struct itype	*find_symb(struct itype *, const struct elfsym *, const char *,
		     int);
void		 dump_type(struct itype *);
void		 dump_func(struct itype *, int *);
void		 dump_obj(struct itype *, int *);

const char	*ctf_enc2name(unsigned short);

static struct elfobj		 eo;
static const char		*strtab;
static const struct elfsym	*symtab;
static size_t			 strtabsz, nsymb;

/* .debug_str and the DWARF 5 indirection tables, read by parse.c */
const char		*dstrbuf, *daddrbuf, *dstroffbuf;
size_t			 dstrlen, daddrlen, dstrofflen;

/* lists of parsed types and functions */
struct itype_queue itypeq = TAILQ_HEAD_INITIALIZER(itypeq);
struct itype_queue ifuncq = TAILQ_HEAD_INITIALIZER(ifuncq);
struct itype_queue iobjq = TAILQ_HEAD_INITIALIZER(iobjq);

/* Target properties, options and statistics shared with parse/generate. */
int		 ctf_version = CTF_VERSION_3;
int		 target_bigendian;
int		 target_reloc;
unsigned int	 target_psz = sizeof(void *);
int		 verbose;
struct ctfstats	 stats;

void		 print_stats(void);

__dead2 void
usage(void)
{
	fprintf(stderr, "usage: %s [-2v] [-l label] [-o outfile] file\n"
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

	while ((ch = getopt(argc, argv, "2dl:o:v")) != -1) {
		switch (ch) {
		case '2':
			ctf_version = CTF_VERSION_2;
			break;
		case 'd':
			dump = 1;	/* ctfdump(1)-like SUNW_ctf sections */
			break;
		case 'l':
			if (label != NULL)
				usage();
			/* An empty label (unset VERSION in a build) is none. */
			label = (*optarg != '\0') ? optarg : NULL;
			break;
		case 'o':
			if (outfile != NULL)
				usage();
			outfile = optarg;
			break;
		case 'v':
			verbose++;
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
	if (error == ELF_NODWARF) {
		/* Nothing to convert: leave the file alone. */
		if (verbose)
			warnx("%s: no DWARF or no C compilation unit in it "
			    "(%lu skipped), file left unchanged", filename,
			    stats.st_cus_skipped);
		return 0;
	}
	if (error != 0)
		return 1;

	if (outfile != NULL) {
		if (generate(dfd, base, outfile, label, 1) != 0)
			return 1;
	} else if (!dump) {
		char	*ctfdata;
		size_t	 ctflen;

		/* In-place mode. */
		if (generate_data(label, 1, &ctfdata, &ctflen) != 0)
			return 1;
		error = elfobj_write_ctf(&eo, dfd, base, ctfdata, ctflen);
		free(ctfdata);
		if (error != 0)
			return 1;
	}

	elfobj_close(&eo);

	if (verbose)
		print_stats();

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
 * Returns 0 on success, ELF_NODWARF if there is nothing to convert (no
 * DWARF, or no C compilation unit in it), -1 on error.  ``fd'' is the
 * input, open for reading.
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
	daddrbuf = eo.addrs;
	daddrlen = eo.addrslen;
	dstroffbuf = eo.stroffs;
	dstrofflen = eo.stroffslen;

	error = dwarf_parse(eo.info, eo.infolen, eo.abbrev, eo.abbrevlen);
	if (error != 0) {
		/* Unreadable debug data is an error, never "no DWARF". */
		if (error == ENOTSUP)
			warnx("%s: DWARF version %u is not supported", path,
			    dw_cu_version);
		else if (error == EOVERFLOW)
			warnx("%s: 64-bit DWARF is not supported", path);
		else
			warnx("%s: malformed DWARF", path);
		elfobj_close(&eo);
		return -1;
	}

	/*
	 * DWARF whose every compilation unit was skipped (assembler
	 * only) is the same as no DWARF.
	 */
	if (stats.st_cus == 0) {
		elfobj_close(&eo);
		return ELF_NODWARF;
	}

	/* Sort functions */
	elf_sort();

	/* Prune unreferenced types, assign final IDs. */
	types_finalize();

	return 0;
}

/*
 * Strip the suffixes the compiler and linker append to symbol names
 * before matching by name: ``.llvm.<digits>'' (ThinLTO promotion), ``.cold'',
 * ``.part.N'', ``.constprop.N'', ``.isra.N''.  Modifies ``sname''.
 */
void
strip_symbol_suffix(char *sname)
{
	static const char *const suffixes[] = {
		".llvm.", ".cold", ".part.", ".constprop.", ".isra."
	};
	char *p;
	size_t i, len;

	for (i = 0; i < nitems(suffixes); i++) {
		len = strlen(suffixes[i]);
		p = sname;
		while ((p = strstr(p, suffixes[i])) != NULL) {
			char *q = p + len;

			/* Dotted suffixes must be followed by digits only. */
			if (suffixes[i][len - 1] == '.') {
				if (*q == '\0')
					break;
				while (*q >= '0' && *q <= '9')
					q++;
			}
			if (*q == '\0' || *q == '.') {
				*p = '\0';
				break;
			}
			p++;
		}
	}
}

/*
 * Guess which part of a function-local static symbol name is the
 * variable name.
 *
 * gcc emits:
 *
 *	varname.id
 *
 * clang emits:
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

static const char *
path_basename(const char *path)
{
	const char *p;

	if (path == NULL)
		return NULL;
	p = strrchr(path, '/');
	return (p != NULL) ? p + 1 : path;
}

/*
 * Find the DWARF entity for a symbol by name (tiers 2 and 3 of the
 * rules above find_symb()).  ``file'' is the name of the nearest preceding STT_FILE symbol
 * for local symbols, NULL otherwise.  Address matching (tier 1) comes
 * with the libelf/libdwarf layer.
 */
struct itype *
find_symb_named(struct itype *tmp, const char *name, const char *file)
{
	struct itype	*it, *match = NULL, *ext = NULL, *any = NULL;
	const char	*fbase = path_basename(file);
	int		 next = 0, nany = 0;

	strlcpy(tmp->it_name, name, ITNAME_MAX);
	tmp->it_cuname = NULL;

	for (it = RB_NFIND(isymb_tree, &isymbt, tmp); it != NULL;
	    it = RB_NEXT(isymb_tree, &isymbt, it)) {
		if (strcmp(it_name(it), name) != 0 ||
		    (it->it_flags & (ITF_FUNC|ITF_OBJ)) !=
		    (tmp->it_flags & (ITF_FUNC|ITF_OBJ)))
			break;

		/* Tier 2: same file, for local symbols. */
		if (fbase != NULL && it->it_cuname != NULL &&
		    strcmp(path_basename(it->it_cuname), fbase) == 0) {
			if (match != NULL)
				return NULL;	/* ambiguous within the CU */
			match = it;
		}
		if (it->it_flags & ITF_EXTERN) {
			ext = it;
			next++;
		}
		any = it;
		nany++;
	}

	if (match != NULL)
		return match;

	/* Tier 3: unique by name, external entities first. */
	if (next == 1)
		return ext;
	if (nany == 1)
		return any;
	return NULL;
}

/*
 * Associating symbols with DWARF entities.  For each qualifying symbol
 * (see elf_sort()), the function or variable it denotes is found in
 * this order, first match wins:
 *
 * 1. Address.  ET_EXEC/ET_DYN: st_value equals the entity's address.
 *    ET_REL: st_value is an offset in section st_shndx, and the entity's
 *    relocated address is likewise (section, offset), so both sides are
 *    compared in one synthetic address space (elf_shbase[]).  Several
 *    entities at one address (aliases, constants merged by the linker):
 *    the one with the symbol's name and file wins, then the one with
 *    its name, else the first.  Address matching is what gets weak
 *    aliases, function-local statics and identical-code-folded
 *    functions right; name matching cannot.
 * 2. Name and file, for local symbols: a LOCAL symbol belongs to the
 *    nearest preceding STT_FILE symbol; a static entity of the CU with
 *    that basename and the symbol's name, when exactly one exists.
 * 3. Name only, for global and weak symbols or when 2 fails: a unique
 *    entity of that name among DW_AT_external ones first, then among
 *    all.  The suffixes of strip_symbol_suffix() are removed first and
 *    the match is accepted only when the stripped name is unique.
 * 4. Otherwise the entry is a placeholder.
 *
 * The previous tools matched by name only, which is where their
 * function-local statics stayed untyped and where same-name statics
 * all got one type.
 */
struct itype *
find_symb(struct itype *tmp, const struct elfsym *st, const char *file,
    int local)
{
	struct itype		*it = NULL, *cand;
	char 			*sname, *p = NULL;
	size_t			 stroff = st->st_name;
	int			 best;

	if (strtab == NULL || stroff >= strtabsz) {
		/* The placeholder must not inherit the previous name. */
		tmp->it_name[0] = '\0';
		return NULL;
	}

	sname = xstrdup(strtab + stroff);
	strip_symbol_suffix(sname);
	/* Function-local statics: clang ``f.x'', gcc ``x.N''. */
	if (local && strchr(sname, '.') != NULL) {
		char *tmpname = xstrdup(sname);

		if ((p = guess_static_local_name(tmpname)) != NULL)
			p = xstrdup(p);
		free(tmpname);
	}

	/* Tier 1: address. */
	if (st->st_shndx != SHN_UNDEF && st->st_shndx < SHN_LORESERVE) {
		tmp->it_addr = st->st_value;
		if (target_reloc && st->st_shndx < elf_nshbase)
			tmp->it_addr += elf_shbase[st->st_shndx];
		it = RB_FIND(iaddr_tree, &iaddrt, tmp);
		/*
		 * Several entities at one address (aliases, identical
		 * constants merged by the linker): prefer the one with
		 * the symbol's name and file, then the one with its
		 * name, else the first.
		 */
		for (cand = it, best = 0; cand != NULL;
		    cand = cand->it_addrnext) {
			int score = 0;

			if (strcmp(it_name(cand), sname) == 0 ||
			    (p != NULL && strcmp(it_name(cand), p) == 0)) {
				score = 1;
				if (file != NULL && cand->it_cuname != NULL &&
				    strcmp(path_basename(cand->it_cuname),
				    path_basename(file)) == 0)
					score = 2;
			}
			if (score > best) {
				best = score;
				it = cand;
			}
		}
		/*
		 * A zero-sized function symbol is an assembler label; it
		 * only takes the type of a function with its own name.
		 */
		if (it != NULL && best == 0 && st->st_size == 0 &&
		    ELF_ST_TYPE(st->st_info) == STT_FUNC)
			it = NULL;
		if (it != NULL) {
			stats.st_sym_byaddr++;
			free(sname);
			free(p);
			return it;
		}
	}

	if (*sname != '\0')
		it = find_symb_named(tmp, sname, file);
	if (it == NULL && p != NULL)
		it = find_symb_named(tmp, p, file);
	free(p);

	if (it != NULL) {
		if (file != NULL && it->it_cuname != NULL)
			stats.st_sym_byfile++;
		else
			stats.st_sym_byname++;
	}

	free(sname);

	/* Restore original name */
	if (it == NULL)
		strlcpy(tmp->it_name, (strtab + stroff), ITNAME_MAX);

	return it;
}

/*
 * The positional symbol contract.  Both symbol sections of the container
 * are parallel to .symtab: the readers (the kernel linker, libctf,
 * libdtrace) walk the symbol table from index 0 and consume one object
 * entry for each qualifying STT_OBJECT symbol and one function record
 * for each qualifying STT_FUNC symbol, in order, never looking at names.
 * An off-by-one silently mistypes every later symbol, and a symbol table
 * rewritten after the container was made (a build step localizing
 * symbols) makes the whole container stale: the conversion must run on
 * the final symbol table.
 *
 * A symbol qualifies iff it has a name, is defined (st_shndx is not
 * SHN_UNDEF), is STT_OBJECT or STT_FUNC, and, for an object, is not an
 * SHN_ABS symbol with value 0.  Binding is irrelevant: local statics and
 * weak aliases consume entries; so do SHN_COMMON and non-zero SHN_ABS
 * objects.  libctf alone skips symbols named _START_ and _END_, Solaris
 * linker artifacts; the kernel does not, and this tool follows the
 * kernel, warning when such a symbol exists.  Without .symtab both
 * sections are empty.
 *
 * Each entry is the type ID of the entity found by find_symb(), or a
 * placeholder (0 for an object, a one-word record of kind UNKNOWN for a
 * function) when none is.  generate.c writes them in this order.
 */
void
elf_sort(void)
{
	struct itype		*it, tmp;
	const char		*file = NULL;
	size_t			 i;
	int			 local;

	memset(&tmp, 0, sizeof(tmp));
	for (i = 0; i < nsymb; i++) {
		const struct elfsym *st = &symtab[i];

		/* Local symbols belong to the preceding STT_FILE symbol. */
		if (ELF_ST_TYPE(st->st_info) == STT_FILE) {
			file = (strtab != NULL && st->st_name < strtabsz) ?
			    strtab + st->st_name : NULL;
			continue;
		}

		/* Qualifying symbols, see the contract above. */
		if (st->st_name == 0 || st->st_shndx == SHN_UNDEF)
			continue;

		switch (ELF_ST_TYPE(st->st_info)) {
		case STT_FUNC:
			tmp.it_flags = ITF_FUNC;
			break;
		case STT_OBJECT:
			if (st->st_shndx == SHN_ABS && st->st_value == 0)
				continue;
			tmp.it_flags = ITF_OBJ;
			break;
		default:
			continue;
		}

		if (strtab != NULL && st->st_name < strtabsz &&
		    (strcmp(strtab + st->st_name, "_START_") == 0 ||
		    strcmp(strtab + st->st_name, "_END_") == 0))
			warnx("symbol %s: readers disagree on whether it "
			    "consumes a CTF entry", strtab + st->st_name);

		local = (ELF_ST_BIND(st->st_info) == STB_LOCAL);
		it = find_symb(&tmp, st, local ? file : NULL, local);
		if (it == NULL) {
			/* Insert 'unknown' entry to match symbol order. */
			it = it_dup(&tmp);
			it->it_refp = it;
			stats.st_sym_untyped++;
			if (verbose > 1)
				warnx("symbol not found: %s (address 0x%"
				    PRIx64 ")", it_name(it), tmp.it_addr);
		} else
			stats.st_sym_typed++;

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

	if (it->it_flags & ITF_FORWARD) {
		printf("  [%u] FORWARD %s\n", it->it_idx, type_name(it));
		return;
	}

	switch (it->it_type) {
	case CTF_K_FLOAT:
	case CTF_K_INTEGER:
		printf("  [%u] %s %s encoding=%s offset=0 bits=%u\n",
		    it->it_idx,
		    (it->it_type == CTF_K_INTEGER) ? "INTEGER" : "FLOAT",
		    it_name(it), ctf_enc2name(it->it_enc), it->it_bits);
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
		    it->it_idx, type_name(it), it->it_refp->it_idx,
		    long_it != NULL ? long_it->it_idx : 0, it->it_nelems);
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
		if (it->it_flags & ITF_VARARGS)
			printf("%s0", it->it_nelems ? ", " : "");
		printf(")\n");
		break;
	default:
		assert(0 == 1);
	}
}

void
print_stats(void)
{
	fprintf(stderr, "compilation units: %lu converted, %lu skipped\n",
	    stats.st_cus, stats.st_cus_skipped);
	fprintf(stderr, "types: %lu parsed, %lu duplicates, %lu pruned, "
	    "%lu written\n", stats.st_types_in, stats.st_types_dup,
	    stats.st_types_pruned, stats.st_types_out);
	fprintf(stderr, "conflicting definitions kept: %lu\n",
	    stats.st_conflicts);
	fprintf(stderr, "forwards: %lu resolved, %lu remaining\n",
	    stats.st_fwd_resolved, stats.st_fwd_left);
	fprintf(stderr, "references to unknown DIEs made void: %lu\n",
	    stats.st_unres);
	fprintf(stderr, "references into other units: %lu\n", stats.st_xrefs);
	fprintf(stderr, "bitfield types: %lu\n", stats.st_bitfields);
	fprintf(stderr, "enumerators outside int32: %lu\n",
	    stats.st_enum_trunc);
	fprintf(stderr, "symbols: %lu typed (%lu by address, %lu by name and "
	    "file, %lu by name), %lu untyped\n", stats.st_sym_typed,
	    stats.st_sym_byaddr, stats.st_sym_byfile, stats.st_sym_byname,
	    stats.st_sym_untyped);
}

void
dump_func(struct itype *it, int *idx)
{
	struct imember *im;

	(*idx)++;

	if (it->it_type == CTF_K_UNKNOWN)
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
