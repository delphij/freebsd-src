/*	$OpenBSD: parse.c,v 1.20 2024/02/22 13:17:18 claudio Exp $ */

/*
 * Copyright (c) 2016-2017 Martin Pieuchot
 * Copyright (c) 2016 Jasper Lievisse Adriaanse <jasper@openbsd.org>
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

/*
 * DWARF to IT (internal type) representation parser.
 */

#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/types.h>

#include <sys/ctf.h>

#include <assert.h>
#include <limits.h>
#include <err.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dwarf.h>

#include "itype.h"
#include "xmalloc.h"
#include "dw.h"
#include "pool.h"

#ifdef DEBUG
#include <stdio.h>
#endif

#ifndef NOPOOL
static struct pool it_pool, im_pool, iom_pool;
#endif /* NOPOOL */

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

#ifdef DEBUG
#define DPRINTF(x...)	do { printf(x); } while (0)
#else
#define DPRINTF(x...)	do { ; } while (0)
#endif

#define VOID_OFFSET	1	/* Fake offset for generating "void" type. */
#define LONG_OFFSET	2	/* Fake offset for the array index type. */

/*
 * Tree used to resolve the types of one unit by their DIE offset.
 */
RB_HEAD(ioff_tree, itype);

/*
 * Out-of-line instances of inline functions carry the address while
 * the abstract DIE carries name and type (likewise GCC's definitions of
 * variables declared earlier): (DIE offset, address) pairs, applied by
 * cu_resolve().
 */
struct iorigin {
	uint64_t	 io_ref;	/* section offset of the DIE */
	uint64_t	 io_addr;
};

/*
 * A compilation unit between its parsing and its merge.  Units are
 * read, resolved and merged in file order; a reference into a later
 * unit (DW_FORM_ref_addr, as ThinLTO's imported units use it) makes
 * cu_lookup() read ahead to that unit.  The units read so far wait
 * here, still in file order, and merge together as one group once
 * every one of them is resolved (dwarf_parse()).
 */
struct cunit {
	TAILQ_ENTRY(cunit)	 cu_next;
	size_t			 cu_off;	/* section offset of the unit */
	size_t			 cu_end;	/* offset of the next unit */
	int			 cu_resolved;	/* cu_resolve() done */
	const char		*cu_name;	/* DW_AT_name */
	struct itype_queue	 cu_types;	/* its types, in ID order */
	struct itype_queue	 cu_aliases;	/* its transparent DIEs */
	struct ioff_tree	 cu_offt;	/* its types by DIE offset */
	struct iorigin		*cu_origins;
	size_t			 cu_norigins, cu_maxorigins;
};
static TAILQ_HEAD(, cunit)	 pendingq = TAILQ_HEAD_INITIALIZER(pendingq);
static struct cunit		*parsing_cu;	/* the unit cu_parse() fills */
static struct dwbuf		 dinfo, dabbrev;/* the rest of the input */
static size_t			 dinfolen;	/* size of .debug_info */
static size_t			 read_end;	/* offset after the last unit read */
static int			 read_error;	/* a unit read ahead was bad */

/*
 * Map from the DIE offsets of the merged units to the types that
 * survived their merge, for DW_FORM_ref_addr references back into
 * them.  Kept only for a file whose abbreviations use that form.
 */
struct ioffmap {
	RB_ENTRY(ioffmap)	 iom_node;
	size_t			 iom_off;
	struct itype		*iom_it;
};
static RB_HEAD(ioffmap_tree, ioffmap) ioffmapt = RB_INITIALIZER(&ioffmapt);
static int			 have_ref_addr;

/*
 * Per-type trees used to merge existing types with the ones of
 * a newly parsed CU.
 */
static RB_HEAD(itype_tree, itype) itypet[CTF_K_MAX];

/*
 * Tree of symbols used to build a list matching the order of
 * the ELF symbol table.
 */
struct isymb_tree	 isymbt;
struct iaddr_tree	 iaddrt;

static struct itype	*void_it;		/* no type is emited for void */
struct itype		*long_it;		/* array index type */
uint32_t		 tidx;			/* type ID */
static uint32_t		 fidx, oidx;		/* func & object IDs */

/*
 * Types synthesized while parsing a DIE (inner dimensions of a
 * multi-dimensional array) that have no DIE of their own.  cu_parse()
 * moves them to the CU queue right after their parent so that the
 * queue stays in type ID order.
 */
static struct itype_queue extraq = TAILQ_HEAD_INITIALIZER(extraq);
static const char	*cu_name;		/* DW_AT_name of current CU */

static struct itype	*cu_lookup(struct cunit *, size_t);
static int		 cu_read(void);
static void		 cu_free(struct cunit *);
static void		 symb_insert(struct itype *);
static void		 iom_add(size_t, struct itype *);
static struct itype	*iom_find(size_t);
static const char	*dstr_at(uint64_t);
static void		 origin_add(uint64_t, uint64_t);
static void		 parse_alias(struct dwdie *, size_t, struct cunit *);
static void		 refers_attrs(struct dwdie *, size_t, const char **,
			     size_t *, size_t *);

/*
 * DWARF 5 indirection: the strx and addrx forms are
 * indices into .debug_str_offsets and .debug_addr relative to the
 * unit's DW_AT_str_offsets_base and DW_AT_addr_base.  cu_bases() takes
 * them from the unit DIE before anything else of the unit is looked
 * at; after ld -r every unit has its own, so the attribute is used,
 * never a fixed header size.
 */
static uint64_t		 cu_stroff_base, cu_addr_base;
static uint64_t		 cu_off;	/* offset of the current CU */

static int		 it_cmp_body(struct itype *, struct itype *);

static void		 cu_bases(struct dwcu *);
static int		 cu_addrx(uint64_t, size_t, uint64_t *);
static const char	*cu_strx(uint64_t);
static int		 dav_is_addrx(struct dwaval *);

/* Comparison mode of it_cmp() for named aggregates and typedefs. */
static int		 it_cmp_identity = 1;

/* Bitfield clones created for the current CU, see it_bitfield(). */
#define BFCACHE_SZ	64
static struct itype	*bfcache[BFCACHE_SZ];
static int		 nbfcache;


void		 cu_stat(void);
void		 cu_parse(struct dwcu *, struct cunit *);
void		 cu_resolve(struct cunit *);
void		 cu_bitfields(struct cunit *);
void		 types_finalize(void);
void		 cu_merge_named(struct cunit *);
void		 cu_merge_anon(struct cunit *);
void		 cu_merge(struct cunit *);
void		 cu_redirect(struct itype_queue *);

struct itype	*parse_base(struct dwdie *, size_t);
struct itype	*parse_refers(struct dwdie *, size_t, int);
struct itype	*parse_array(struct dwdie *, size_t);
struct itype	*parse_enum(struct dwdie *, size_t);
struct itype	*parse_struct(struct dwdie *, size_t, int, size_t);
struct itype	*parse_function(struct dwdie *, size_t);
struct itype	*parse_funcptr(struct dwdie *, size_t);
struct itype	*parse_variable(struct dwdie *, size_t);

void		 subparse_subrange(struct dwdie *, size_t, struct itype *);
void		 subparse_enumerator(struct dwdie *, size_t, struct itype *);
void		 subparse_member(struct dwdie *, size_t, struct itype *, size_t);
void		 subparse_arguments(struct dwdie *, size_t, struct itype *);

size_t		 dav2val(struct dwaval *, size_t);
const char	*dav2str(struct dwaval *);
const char	*canon_int_name(const char *, uint16_t *);
int		 cu_language_ok(struct dwdie *, size_t);
struct itype	*it_base(struct itype *);
struct itype	*it_bitfield(struct itype *, uint32_t);
int		 it_cmp_def(struct itype *, struct itype *);
int		 im_cmp(struct imember *, struct imember *, int);

struct itype	*it_new(uint64_t, size_t, const char *, uint32_t, uint16_t,
		     uint64_t, uint16_t, unsigned int);
void		 it_merge(struct itype *, struct itype *);
void		 it_reference(struct itype *);
void		 it_free(struct itype *);
int		 it_cmp(struct itype *, struct itype *);
int		 it_name_cmp(struct itype *, struct itype *);
int		 it_addr_cmp(struct itype *, struct itype *);
int		 it_off_cmp(struct itype *, struct itype *);
int		 iom_cmp(struct ioffmap *, struct ioffmap *);
struct imember	*im_new(const char *, size_t, size_t);

RB_GENERATE_STATIC(itype_tree, itype, it_node, it_cmp);
RB_GENERATE(isymb_tree, itype, it_node, it_name_cmp);
RB_GENERATE(iaddr_tree, itype, it_anode, it_addr_cmp);
RB_GENERATE_STATIC(ioff_tree, itype, it_node, it_off_cmp);
RB_GENERATE_STATIC(ioffmap_tree, ioffmap, iom_node, iom_cmp);

/*
 * Construct a list of internal type and functions based on DWARF
 * INFO and ABBREV sections.
 *
 * Multiple CUs are supported.  Returns 0, or the error of the unit
 * that could not be read: ENOTSUP for a DWARF version the reader does
 * not handle (dw_cu_version says which), EOVERFLOW for 64-bit DWARF,
 * anything else for malformed data.  The caller reports it; a unit the
 * reader cannot parse must never pass for "no DWARF".
 */
int
dwarf_parse(const char *infobuf, size_t infolen, const char *abbuf,
    size_t ablen)
{
	struct cunit	*cu;
	int		 i, error;

	dinfo.buf = infobuf;
	dinfo.len = dinfolen = infolen;
	dabbrev.buf = abbuf;
	dabbrev.len = ablen;
	read_end = 0;
	read_error = 0;

	for (i = 0; i < CTF_K_MAX; i++)
		RB_INIT(&itypet[i]);
	RB_INIT(&isymbt);

	/*
	 * DW_FORM_ref_addr is the only form that refers across units
	 * (ThinLTO's imported units use it); a file whose abbreviations
	 * never use it keeps the cross-unit machinery, and its memory,
	 * off.
	 */
	have_ref_addr = dw_ab_uses_form(&dabbrev, DW_FORM_ref_addr);

	void_it = it_new(++tidx, VOID_OFFSET, "void", 0,
	    CTF_INT_SIGNED, 0, CTF_K_INTEGER, 0);
	void_it->it_flags &= ~ITF_PENDING;
	TAILQ_INSERT_TAIL(&itypeq, void_it, it_next);

	/*
	 * In file order: read a unit (cu_read() parses it into types
	 * right away), resolve its references, merge its types with
	 * those seen so far.  A reference into a later unit reads that
	 * unit ahead into pendingq, and the units there form one group:
	 * all are resolved (and their bitfield clones made) before any
	 * merges, so that no comparison meets an unresolved type, and
	 * the merge runs each of its passes over the whole group, so
	 * that a reference into the group has reached its surviving
	 * type before anything compares through it (see cu_merge()).
	 * Without such references the group is one unit, and this is
	 * the plain one-at-a-time merge.
	 */
	while (!TAILQ_EMPTY(&pendingq) || dinfo.len > 0) {
		if (TAILQ_EMPTY(&pendingq) && (error = cu_read()) != 0)
			return error;

		/* Resolving may read ahead and lengthen the queue. */
		TAILQ_FOREACH(cu, &pendingq, cu_next) {
			if (cu->cu_resolved)
				continue;
			cu_resolve(cu);
			if (read_error != 0)
				return read_error;
		}
		TAILQ_FOREACH(cu, &pendingq, cu_next)
			cu_bitfields(cu);

#ifdef DEBUG
		cu_stat();
#endif
		TAILQ_FOREACH(cu, &pendingq, cu_next)
			cu_merge_named(cu);
		TAILQ_FOREACH(cu, &pendingq, cu_next)
			cu_redirect(&cu->cu_types);
		TAILQ_FOREACH(cu, &pendingq, cu_next)
			cu_merge_anon(cu);
		TAILQ_FOREACH(cu, &pendingq, cu_next)
			cu_redirect(&cu->cu_types);
		while ((cu = TAILQ_FIRST(&pendingq)) != NULL) {
			TAILQ_REMOVE(&pendingq, cu, cu_next);
			cu_merge(cu);
			cu_free(cu);
		}
	}

	return 0;
}

/*
 * Read the next unit of the file and queue it as pending: its DIEs
 * are parsed into types at once and freed, the unit keeps only what
 * struct cunit holds.  A unit that is not a C compile unit stays empty,
 * so that a reference into it finds nothing.
 */
static int
cu_read(void)
{
	struct dwcu	*dcu = NULL;
	struct cunit	*cu;
	int		 error;

	error = dw_cu_parse(&dinfo, &dabbrev, dinfolen, &dcu);
	if (error != 0)
		return error;

	cu = xcalloc(1, sizeof(*cu));
	TAILQ_INIT(&cu->cu_types);
	TAILQ_INIT(&cu->cu_aliases);
	RB_INIT(&cu->cu_offt);
	cu->cu_off = dcu->dcu_offset;
	cu->cu_end = dinfolen - dinfo.len;
	read_end = cu->cu_end;

	if (dcu->dcu_unit_type != 0 && dcu->dcu_unit_type != DW_UT_compile) {
		/* Type, skeleton and split units carry nothing for us. */
		stats.st_cus_skipped++;
	} else {
		cu_bases(dcu);
		target_psz = dcu->dcu_psize;

		/*
		 * Arrays are indexed by a synthesized ``long'' of the
		 * target's address size.  A real ``long''
		 * found later merges into it.
		 */
		if (long_it == NULL) {
			long_it = it_new(++tidx, LONG_OFFSET, "long",
			    target_psz, CTF_INT_SIGNED, 0, CTF_K_INTEGER, 0);
			long_it->it_bits = 8 * target_psz;
			long_it->it_flags &= ~ITF_PENDING;
			TAILQ_INSERT_TAIL(&itypeq, long_it, it_next);
			RB_INSERT(itype_tree, &itypet[CTF_K_INTEGER], long_it);
		}

		parsing_cu = cu;
		cu_parse(dcu, cu);
		parsing_cu = NULL;
		cu->cu_name = cu_name;
	}
	dw_dcu_free(dcu);

	TAILQ_INSERT_TAIL(&pendingq, cu, cu_next);
	return 0;
}

/*
 * The pending unit holding section offset ``off'', reading units
 * until one covers it; NULL past the end of the file or when a unit
 * could not be read (read_error says so, dwarf_parse() reports it).
 */
static struct cunit *
cu_ahead(size_t off)
{
	struct cunit *cu;

	while (off >= read_end && dinfo.len > 0 && read_error == 0)
		read_error = cu_read();
	TAILQ_FOREACH(cu, &pendingq, cu_next) {
		if (off >= cu->cu_off && off < cu->cu_end)
			return cu;
	}
	return NULL;
}

static void
cu_free(struct cunit *cu)
{
	struct itype *it;

	/* The transparent DIEs have served; nothing refers to them now. */
	while ((it = TAILQ_FIRST(&cu->cu_aliases)) != NULL) {
		TAILQ_REMOVE(&cu->cu_aliases, it, it_next);
		it_free(it);
	}
	free(cu->cu_origins);
	free(cu);
}

struct itype *
it_new(uint64_t index, size_t off, const char *name, uint32_t size,
    uint16_t enc, uint64_t ref, uint16_t type, unsigned int flags)
{
	struct itype *it;
#ifndef NOPOOL
	static int it_pool_inited = 0;

	if (!it_pool_inited) {
		pool_init(&it_pool, "it", 512, sizeof(struct itype));
		pool_init(&im_pool, "im", 1024, sizeof(struct imember));
		pool_init(&iom_pool, "iom", 1024, sizeof(struct ioffmap));
		it_pool_inited = 1;
	}
#endif

	assert((name != NULL) || !(flags & (ITF_FUNC|ITF_OBJ)));

	it = pmalloc(&it_pool, sizeof(*it));
	TAILQ_INIT(&it->it_members);
	it->it_off = off;
	it->it_ref = ref;
	it->it_addr = 0;
	it->it_refp = NULL;
	it->it_survivor = NULL;
	it->it_alt = NULL;
	it->it_addrnext = NULL;
	it->it_cuname = cu_name;
	it->it_size = size;
	it->it_bits = 0;
	it->it_nelems = 0;
	it->it_enc = enc;
	it->it_idx = index;
	it->it_type = type;
	it->it_flags = flags | ITF_PENDING;

	if (name == NULL) {
		it->it_flags |= ITF_ANON;
	} else {
		size_t n;

		if ((n = strlcpy(it->it_name, name, ITNAME_MAX)) > ITNAME_MAX)
			warnx("name %s too long %zd > %d", name, n, ITNAME_MAX);
	}

	return it;
}

struct itype *
it_dup(struct itype *it)
{
	struct imember *copim, *im;
	struct itype *copit;

	copit = it_new(it->it_idx, it->it_off, it_name(it), it->it_size,
	    it->it_enc, it->it_ref, it->it_type, it->it_flags);

	copit->it_refp = it->it_refp;
	copit->it_nelems = it->it_nelems;
	copit->it_bits = it->it_bits;
	copit->it_addr = it->it_addr;

	TAILQ_FOREACH(im, &it->it_members, im_next) {
		copim = im_new(im_name(im), im->im_ref, im->im_off);
		copim->im_refp = im->im_refp;
		copim->im_bits = im->im_bits;
		copim->im_bitoff = im->im_bitoff;
		copim->im_flags |= im->im_flags & IMF_BITOFF;
		TAILQ_INSERT_TAIL(&copit->it_members, copim, im_next);
	}

	return copit;
}

/*
 * Merge the content of ``it'', the full type declaration into the
 * forwarding representation ``fwd''.
 */
void
it_merge(struct itype *fwd, struct itype *it)
{
	assert(fwd->it_flags & ITF_FORWARD);
	assert(fwd->it_type == it->it_type);
	assert(TAILQ_EMPTY(&fwd->it_members));

	fwd->it_off = it->it_off;
	fwd->it_ref = it->it_ref;
	fwd->it_refp = it->it_refp;
	fwd->it_size = it->it_size;
	fwd->it_bits = it->it_bits;
	fwd->it_nelems = it->it_nelems;
	fwd->it_enc = it->it_enc;
	fwd->it_flags = (it->it_flags & ~ITF_PENDING) |
	    (fwd->it_flags & ITF_PENDING);

	TAILQ_CONCAT(&fwd->it_members, &it->it_members, im_next);
	assert(TAILQ_EMPTY(&it->it_members));
}

const char *
it_name(struct itype *it)
{
	if (!(it->it_flags & ITF_ANON))
		return it->it_name;

	return NULL;
}

void
it_reference(struct itype *it)
{
	struct imember *im;

	if (it == NULL || it->it_flags & ITF_USED)
		return;

	it->it_flags |= ITF_USED;

	if (it->it_type == CTF_K_ARRAY && long_it != NULL)
		long_it->it_flags |= ITF_USED;

	it_reference(it->it_refp);
	TAILQ_FOREACH(im, &it->it_members, im_next)
		it_reference(im->im_refp);
}

void
it_free(struct itype *it)
{
	struct imember *im;

	if (it == NULL)
		return;

	while ((im = TAILQ_FIRST(&it->it_members)) != NULL) {
		TAILQ_REMOVE(&it->it_members, im, im_next);
		pfree(&im_pool, im);
	}

	pfree(&it_pool, it);
}

/*
 * Compare two members: name, offset and the identity of the member
 * type; for enumerators (``deep'' clear) name and value, since im_ref
 * holds the value and there is no type.
 */
int
im_cmp(struct imember *a, struct imember *b, int deep)
{
	int diff;

	if ((a->im_flags & IMF_ANON) != (b->im_flags & IMF_ANON))
		return (a->im_flags & IMF_ANON) ? -1 : 1;
	if (!(a->im_flags & IMF_ANON) &&
	    (diff = strcmp(a->im_name, b->im_name)) != 0)
		return diff;

	if (a->im_off > b->im_off)
		return 1;
	if (a->im_off < b->im_off)
		return -1;

	/* Enumerators: im_ref holds the value and there is no type. */
	if (!deep) {
		if (a->im_ref > b->im_ref)
			return 1;
		if (a->im_ref < b->im_ref)
			return -1;
		return 0;
	}

	if (a->im_refp != NULL && b->im_refp != NULL)
		return it_cmp(a->im_refp, b->im_refp);
	if (a->im_refp == NULL && b->im_refp == NULL)
		return 0;
	return (a->im_refp == NULL) ? -1 : 1;
}

/*
 * Compare the definitions of two types of the same kind, ignoring
 * their names: size, member list, referenced types.
 */
int
it_cmp_def(struct itype *a, struct itype *b)
{
	int diff;

	/*
	 * Well-formed DWARF has no cycle through anonymous types, since
	 * nothing can name them, so meeting on the left a type that is
	 * already being compared on the left of this path (or likewise on
	 * the right) is a malformed input: order the pair arbitrarily but
	 * deterministically instead of recursing forever.  The two sides
	 * are marked apart: comparing ``char **'' with ``char *'' descends
	 * into the right type from the left, and that is no cycle.
	 */
	if ((a->it_flags & ITF_VISITL) || (b->it_flags & ITF_VISITR)) {
		if (a->it_off != b->it_off)
			return (a->it_off > b->it_off) ? 1 : -1;
		return 0;
	}
	a->it_flags |= ITF_VISITL;
	b->it_flags |= ITF_VISITR;
	diff = it_cmp_body(a, b);
	a->it_flags &= ~ITF_VISITL;
	b->it_flags &= ~ITF_VISITR;
	return diff;
}

static int
it_cmp_body(struct itype *a, struct itype *b)
{
	struct imember *ima, *imb;
	int diff, deep;

	if ((a->it_flags & ITF_FORWARD) != (b->it_flags & ITF_FORWARD))
		return (a->it_flags & ITF_FORWARD) ? -1 : 1;

	if (a->it_size > b->it_size)
		return 1;
	if (a->it_size < b->it_size)
		return -1;
	if (a->it_nelems > b->it_nelems)
		return 1;
	if (a->it_nelems < b->it_nelems)
		return -1;
	if ((a->it_flags & ITF_VARARGS) != (b->it_flags & ITF_VARARGS))
		return (a->it_flags & ITF_VARARGS) ? 1 : -1;

	/* Enumerators carry their value in im_ref and have no type. */
	deep = (a->it_type != CTF_K_ENUM);
	ima = TAILQ_FIRST(&a->it_members);
	imb = TAILQ_FIRST(&b->it_members);
	while (ima != NULL && imb != NULL) {
		if ((diff = im_cmp(ima, imb, deep)) != 0)
			return diff;
		ima = TAILQ_NEXT(ima, im_next);
		imb = TAILQ_NEXT(imb, im_next);
	}
	if (ima != NULL || imb != NULL)
		return (ima == NULL) ? -1 : 1;

	/* Return type, contents or target. */
	if (a->it_refp != NULL && b->it_refp != NULL)
		return it_cmp(a->it_refp, b->it_refp);
	if (a->it_refp == NULL && b->it_refp == NULL)
		return 0;
	return (a->it_refp == NULL) ? -1 : 1;
}

/*
 * Return 0 if ``a'' matches ``b''.
 *
 * This implements type identity (see cu_merge()): a reference to a
 * named struct, union, enum or typedef is identified by its name only,
 * which breaks cycles and lets a forward declaration match its
 * definition; everything else is compared structurally.
 */
int
it_cmp(struct itype *a, struct itype *b)
{
	if (a->it_type > b->it_type)
		return 1;
	if (a->it_type < b->it_type)
		return -1;

	/* Basic types need to have the same encoding and size. */
	if ((a->it_type == CTF_K_INTEGER || a->it_type == CTF_K_FLOAT)) {
		if (a->it_enc > b->it_enc)
			return 1;
		if (a->it_enc < b->it_enc)
			return -1;
		if (a->it_size > b->it_size)
			return 1;
		if (a->it_size < b->it_size)
			return -1;
		if (a->it_bits > b->it_bits)
			return 1;
		if (a->it_bits < b->it_bits)
			return -1;
	}

	/* Match by name */
	if (!(a->it_flags & ITF_ANON) && !(b->it_flags & ITF_ANON)) {
		/*
		 * Once the named definitions of a CU have been merged
		 * (cu_merge() first pass), references to them point at
		 * master types and two same-name definitions with
		 * different contents are told apart by identity.
		 */
		if (it_cmp_identity && (a->it_type == CTF_K_STRUCT ||
		    a->it_type == CTF_K_UNION || a->it_type == CTF_K_ENUM ||
		    a->it_type == CTF_K_TYPEDEF)) {
			if (a == b)
				return 0;
			if (a->it_idx != b->it_idx)
				return (a->it_idx > b->it_idx) ? 1 : -1;
			return (a > b) ? 1 : -1;
		}
		return strcmp(it_name(a), it_name(b));
	}

	/* Only one of them is anonym */
	if ((a->it_flags & ITF_ANON) != (b->it_flags & ITF_ANON))
		return (a->it_flags & ITF_ANON) ? -1 : 1;

	/* Both anonymous: compare the definitions. */
	return it_cmp_def(a, b);
}

/*
 * Symbol tree order: name, kind, then CU name with NULL first so that
 * RB_NFIND() on a key without CU name lands on the first candidate.
 */
int
it_name_cmp(struct itype *a, struct itype *b)
{
	int diff;

	if ((diff = strcmp(it_name(a), it_name(b))) != 0)
		return diff;

	diff = (a->it_flags & (ITF_FUNC|ITF_OBJ)) -
	    (b->it_flags & (ITF_FUNC|ITF_OBJ));
	if (diff != 0)
		return diff;

	if (a->it_cuname == NULL && b->it_cuname == NULL)
		return 0;
	if (a->it_cuname == NULL)
		return -1;
	if (b->it_cuname == NULL)
		return 1;
	return strcmp(a->it_cuname, b->it_cuname);
}

int
it_addr_cmp(struct itype *a, struct itype *b)
{
	if (a->it_addr != b->it_addr)
		return (a->it_addr > b->it_addr) ? 1 : -1;
	return ((a->it_flags & (ITF_FUNC|ITF_OBJ)) -
	    (b->it_flags & (ITF_FUNC|ITF_OBJ)));
}

int
it_off_cmp(struct itype *a, struct itype *b)
{
	return a->it_off - b->it_off;
}

int
iom_cmp(struct ioffmap *a, struct ioffmap *b)
{
	if (a->iom_off != b->iom_off)
		return (a->iom_off > b->iom_off) ? 1 : -1;
	return 0;
}

/*
 * The offset map of the merged units: a DIE maps once, to the type
 * that stands for it.
 */
static void
iom_add(size_t off, struct itype *it)
{
	struct ioffmap *iom;

	iom = pmalloc(&iom_pool, sizeof(*iom));
	iom->iom_off = off;
	iom->iom_it = it;
	if (RB_INSERT(ioffmap_tree, &ioffmapt, iom) != NULL)
		pfree(&iom_pool, iom);
}

static struct itype *
iom_find(size_t off)
{
	struct ioffmap tmp, *iom;

	tmp.iom_off = off;
	iom = RB_FIND(ioffmap_tree, &ioffmapt, &tmp);
	return (iom != NULL) ? iom->iom_it : NULL;
}

/*
 * A function or object joins the symbol trees: by name unless local,
 * and by address when it has one.  Several entities at one address
 * (aliases, identical constants merged by the linker) are all kept, in
 * order.  Done when its unit merges, or later for an entity that gets
 * its address from an instance in a later unit.
 */
static void
symb_insert(struct itype *it)
{
	struct itype *prev;

	if (it->it_flags & ITF_LOCAL)
		return;

	/*
	 * FIXME: allow static variables with the same name to be of
	 * different type.
	 */
	if (RB_FIND(isymb_tree, &isymbt, it) == NULL)
		RB_INSERT(isymb_tree, &isymbt, it);

	if (it->it_flags & ITF_ADDR) {
		if (verbose > 2)
			warnx("entity %s at 0x%" PRIx64, it_name(it),
			    it->it_addr);
		prev = RB_FIND(iaddr_tree, &iaddrt, it);
		if (prev == NULL)
			RB_INSERT(iaddr_tree, &iaddrt, it);
		else {
			while (prev->it_addrnext != NULL)
				prev = prev->it_addrnext;
			prev->it_addrnext = it;
		}
	}
}

struct imember *
im_new(const char *name, size_t ref, size_t off)
{
	struct imember *im;

	im = pmalloc(&im_pool, sizeof(*im));
	im->im_ref = ref;
	im->im_off = off;
	im->im_bits = 0;
	im->im_bitoff = 0;
	im->im_refp = NULL;
	if (name == NULL) {
		im->im_flags = IMF_ANON;
	} else {
		size_t n;

		n = strlcpy(im->im_name, name, ITNAME_MAX);
		if (n > ITNAME_MAX)
			warnx("name %s too long %zd > %d", name, n,
			    ITNAME_MAX);
		im->im_flags = 0;
	}

	return im;
}

const char *
im_name(struct imember *im)
{
	if (!(im->im_flags & IMF_ANON))
		return im->im_name;

	return NULL;
}

void
cu_stat(void)
{
#ifndef NOPOOL
	pool_dump();
#endif
}

/*
 * Resolve the references of a unit: every unresolved type and member
 * gets the type its DIE offset names, through cu_lookup(), which sees
 * the unit itself, the units merged before it and the units after it.
 */
void
cu_resolve(struct cunit *cu)
{
	struct itype_queue *cutq = &cu->cu_types;
	struct itype	*it, *ref;
	struct imember	*im;
	size_t		 i, off = cu->cu_off;

	cu->cu_resolved = 1;

	TAILQ_FOREACH(it, cutq, it_next) {
		if (!(it->it_flags & (ITF_UNRES|ITF_UNRES_MEMB)))
			continue;

		/* If this type references another one, try to find it. */
		if (it->it_flags & ITF_UNRES) {
			ref = cu_lookup(cu, it->it_ref + off);
			if (ref == NULL) {
				/*
				 * A reference to no DIE the reader knows:
				 * void, counted, never a
				 * dangling pointer for the writer.
				 */
				ref = void_it;
				stats.st_unres++;
				if (verbose > 1)
					warnx("%s: reference to unknown DIE "
					    "0x%zx made void",
					    it_name(it) != NULL ? it_name(it) :
					    "(anon)", it->it_ref + off);
			}
			it->it_refp = ref;
			it->it_flags &= ~ITF_UNRES;
		}

		/* If this type has members, resolve all of them. */
		if (it->it_flags & ITF_UNRES_MEMB) {
			TAILQ_FOREACH(im, &it->it_members, im_next) {
				if (im->im_refp != NULL)
					continue;
				ref = cu_lookup(cu, im->im_ref + off);
				if (ref == NULL) {
					ref = void_it;
					stats.st_unres++;
					if (verbose > 1)
						warnx("%s: member %s: reference "
						    "to unknown DIE 0x%zx made "
						    "void", it_name(it) != NULL ?
						    it_name(it) : "(anon)",
						    im_name(im) != NULL ?
						    im_name(im) : "(anon)",
						    im->im_ref + off);
				}
				im->im_refp = ref;
			}
			it->it_flags &= ~ITF_UNRES_MEMB;
		}
	}

	/*
	 * Transparent DIEs nothing looked through yet: resolved now, so
	 * that the offset map can stand in for them later.
	 */
	TAILQ_FOREACH(it, &cu->cu_aliases, it_next)
		if (it->it_refp == NULL)
			it->it_refp = cu_lookup(cu, it->it_ref + off);

	/*
	 * Give abstract inline functions the address of their instance,
	 * and declarations of variables the address of their definition
	 * (DW_AT_specification, GCC): with an address the declaration is
	 * the symbol's entity, no longer a local.  An entity of a unit
	 * merged earlier joins the symbol trees now; one of this group
	 * does so when its unit merges.
	 */
	for (i = 0; i < cu->cu_norigins; i++) {
		ref = cu_lookup(cu, cu->cu_origins[i].io_ref);
		if (ref != NULL && (ref->it_flags & (ITF_FUNC|ITF_OBJ)) &&
		    !(ref->it_flags & ITF_ADDR)) {
			ref->it_addr = cu->cu_origins[i].io_addr;
			ref->it_flags |= ITF_ADDR;
			if (ref->it_flags & ITF_OBJ)
				ref->it_flags &= ~ITF_LOCAL;
			if (!(ref->it_flags & ITF_PENDING))
				symb_insert(ref);
		}
	}
}

/*
 * Bitfield members reference a narrowed clone of their integer type.
 * Done once every unit of the group is resolved, so that typedef and
 * qualifier chains can be followed into any of them.  Clones are
 * appended to the unit's queue; the merge deduplicates them like any
 * other type.
 */
void
cu_bitfields(struct cunit *cu)
{
	struct itype_queue *cutq = &cu->cu_types;
	struct itype	*it, *ref;
	struct imember	*im;

	cu_name = cu->cu_name;
	nbfcache = 0;
	TAILQ_FOREACH(it, cutq, it_next) {
		if (it->it_type != CTF_K_STRUCT && it->it_type != CTF_K_UNION)
			continue;
		TAILQ_FOREACH(im, &it->it_members, im_next) {
			if (im->im_bits == 0 || im->im_refp == NULL)
				continue;

			if (im->im_flags & IMF_BITOFF) {
				struct itype *base = it_base(im->im_refp);
				uint64_t storage;

				storage = (base != NULL) ? base->it_bits : 0;
				if (storage >= im->im_bitoff + im->im_bits)
					im->im_off += storage - im->im_bitoff -
					    im->im_bits;
				else
					warnx("%s: member %s: cannot place "
					    "bitfield", it_name(it) != NULL ?
					    it_name(it) : "(anon)",
					    im_name(im) != NULL ? im_name(im) :
					    "(anon)");
				im->im_flags &= ~IMF_BITOFF;
			}

			ref = it_bitfield(im->im_refp, im->im_bits);
			if (ref == NULL || ref == im->im_refp)
				continue;
			if (!(ref->it_flags & ITF_INSERTED)) {
				ref->it_flags |= ITF_INSERTED;
				TAILQ_INSERT_TAIL(cutq, ref, it_next);
			}
			im->im_refp = ref;
		}
	}
}

/*
 * The type at section offset ``off'' as seen from unit ``cu'': in the
 * unit itself, in a unit merged earlier (through the offset map, kept
 * only when the file uses DW_FORM_ref_addr), in another unit of the
 * pending group or in a later unit, read ahead if need be.
 * Transparent DIEs are looked through, resolved on first sight.  NULL
 * when no type is there.
 */
static struct itype *
cu_lookup_hops(struct cunit *cu, size_t off, int hops)
{
	struct cunit	*tcu;
	struct itype	 tmp, *ref;

	if (hops > 8)
		return NULL;	/* a cycle of transparent DIEs */

	if (off >= cu->cu_off && off < cu->cu_end)
		tcu = cu;
	else if (!have_ref_addr)
		return NULL;
	else if (off < TAILQ_FIRST(&pendingq)->cu_off) {
		/* A unit merged earlier; the map looks through aliases. */
		ref = iom_find(off);
		if (ref != NULL)
			stats.st_xrefs++;
		return ref;
	} else if ((tcu = cu_ahead(off)) == NULL)
		return NULL;

	tmp.it_off = off;
	ref = RB_FIND(ioff_tree, &tcu->cu_offt, &tmp);
	if (ref != NULL && (ref->it_flags & ITF_ALIAS)) {
		if (ref->it_refp == NULL)
			ref->it_refp = cu_lookup_hops(tcu,
			    ref->it_ref + tcu->cu_off, hops + 1);
		ref = ref->it_refp;
	}
	if (ref != NULL && tcu != cu)
		stats.st_xrefs++;
	return ref;
}

static struct itype *
cu_lookup(struct cunit *cu, size_t off)
{
	return cu_lookup_hops(cu, off, 0);
}

/*
 * Return the ``<name> bitfield'' clone of the integer behind ``base''
 * with a width of ``bits'', creating it if needed.  Returns ``base''
 * when no clone is needed and NULL when the type is not an integer.
 */
struct itype *
it_base(struct itype *it)
{
	int depth;

	for (depth = 0; it != NULL && depth < 64; depth++) {
		switch (it->it_type) {
		case CTF_K_TYPEDEF:
		case CTF_K_CONST:
		case CTF_K_VOLATILE:
		case CTF_K_RESTRICT:
			it = it->it_refp;
			continue;
		case CTF_K_INTEGER:
			return it;
		default:
			return NULL;
		}
	}

	return NULL;
}

struct itype *
it_bitfield(struct itype *ref, uint32_t bits)
{
	struct itype *base, *it;
	char name[ITNAME_MAX];
	int i;

	base = it_base(ref);
	if (base == NULL)
		return NULL;
	if (base->it_bits == bits)
		return ref;

	/* Small per-CU cache; cu_merge() catches what it misses. */
	snprintf(name, sizeof(name), "%s bitfield", it_name(base));
	for (i = 0; i < nbfcache; i++) {
		it = bfcache[i];
		if (it->it_bits == bits && it->it_enc == base->it_enc &&
		    it->it_size == base->it_size &&
		    strcmp(it->it_name, name) == 0)
			return it;
	}

	it = it_new(++tidx, 0, name, base->it_size, base->it_enc, 0,
	    CTF_K_INTEGER, 0);
	it->it_bits = bits;
	stats.st_bitfields++;

	if (nbfcache < BFCACHE_SZ)
		bfcache[nbfcache++] = it;

	return it;
}

/*
 * Decide what is emitted once the symbols are matched:
 * everything reachable from a symbol that has a type, from a named
 * struct, union or typedef definition wherever it is declared, or from
 * any enum.  The rest (types only used by local or unnamed variables
 * or by inlined functions without a symbol, array index and enum
 * underlying types, forwards nothing points to) is pruned: nothing in
 * D could name it.  This matches the previous tools as observed on the
 * fixtures and on kernel objects, except that they emit anonymous
 * enums nothing references only some of the time.  IDs are then made
 * contiguous.
 */
void
types_finalize(void)
{
	struct itype *it, *nit;

	TAILQ_FOREACH(it, &iobjq, it_symb) {
		if (it->it_type != CTF_K_UNKNOWN || it->it_refp != it)
			it_reference(it);
	}
	TAILQ_FOREACH(it, &ifuncq, it_symb) {
		if (it->it_type != CTF_K_UNKNOWN)
			it_reference(it);
	}

	TAILQ_FOREACH(it, &itypeq, it_next) {
		if (it->it_flags & (ITF_FUNC|ITF_OBJ|ITF_FORWARD))
			continue;
		switch (it->it_type) {
		case CTF_K_STRUCT:
		case CTF_K_UNION:
		case CTF_K_TYPEDEF:
			if (it->it_flags & ITF_ANON)
				break;
			it_reference(it);
			break;
		case CTF_K_ENUM:
			/* Anonymous too: D resolves enumerator names. */
			it_reference(it);
			break;
		default:
			break;
		}
	}

	tidx = 0;
	for (it = TAILQ_FIRST(&itypeq); it != NULL; it = nit) {
		nit = TAILQ_NEXT(it, it_next);

		if (it->it_flags & (ITF_FUNC|ITF_OBJ))
			continue;

		if (it->it_flags & ITF_USED) {
			it->it_idx = ++tidx;
			continue;
		}

		stats.st_types_pruned++;
		if (verbose > 2)
			warnx("pruned: kind %u %s", it->it_type,
			    it_name(it) != NULL ? it_name(it) : "(anon)");
		if (it == void_it)
			void_it = NULL;
		if (it == long_it)
			long_it = NULL;
		TAILQ_REMOVE(&itypeq, it, it_next);
		it_free(it);
	}
}

/*
 * Redirect references to dropped types to their survivors.  A forward
 * completed from this CU took over the members of a dropped type, so
 * it needs the same treatment.
 */
void
cu_redirect(struct itype_queue *cutq)
{
	struct itype *it, *tgt;
	struct imember *im;

	TAILQ_FOREACH(it, cutq, it_next) {
		tgt = it;
		if (it->it_flags & ITF_DROPPED) {
			if (!(it->it_flags & ITF_MERGED))
				continue;
			tgt = it->it_survivor;
		}

		while (tgt->it_refp != NULL &&
		    (tgt->it_refp->it_flags & ITF_DROPPED))
			tgt->it_refp = tgt->it_refp->it_survivor;
		TAILQ_FOREACH(im, &tgt->it_members, im_next) {
			while (im->im_refp != NULL &&
			    (im->im_refp->it_flags & ITF_DROPPED))
				im->im_refp = im->im_refp->it_survivor;
		}
	}
}

/*
 * Merge type representation from a CU with already known types, in
 * three passes: cu_merge_named(), cu_merge_anon(), cu_merge().  A
 * group of units referring into each other (see dwarf_parse()) runs
 * each pass over every unit before the next pass, with references to
 * dropped types redirected in between (cu_redirect()), so that by the
 * time the structural pass compares a reference to a named type by
 * identity, that type is a master whichever unit of the group defined
 * it, and by the time duplicates are freed nothing points at them.
 *
 * Deduplication, conflicts and forwards:
 *
 * - Two DWARF types are one CTF type iff their canonical descriptions
 *   are equal, computed bottom-up: kind, name, size and encoding for
 *   integers and floats; kind, name (typedef) and the identity of the
 *   target for pointers, qualifiers and typedefs; contents, index and
 *   count for arrays; return, arguments and varargs for functions;
 *   kind, tag, size and the ordered members (name, offset, identity of
 *   type) for structs and unions; name, size and the enumerators for
 *   enums; kind and tag for forwards.  The identity of a reference to a
 *   *named* struct, union or enum is its (kind, tag) only, never the
 *   expanded definition: that breaks cycles and makes a reference the
 *   same whether the tag is complete or forward in that CU.  Anonymous
 *   aggregates and everything else expand recursively (it_cmp()).
 * - Named definitions: several CUs may define one struct, union, enum
 *   or typedef name differently (a driver's private struct, a typedef
 *   that depends on an option).  Every definition whose content differs
 *   from those already seen is kept as a distinct type, as the previous
 *   merger did; identical ones merge.  References from each CU resolve
 *   to that CU's definition, so a D script reaching a type through a
 *   symbol or a member sees the right one, while a lookup by name gives
 *   the first.  Conflicts are counted and named under -v.  (A first-wins
 *   rule would silently retype every variable of the other definitions.)
 * - Forwards: a forward whose (kind, tag) has a definition anywhere in
 *   the file resolves to that definition and is not emitted; the rest
 *   are emitted as FORWARD with the real kind (the previous tools wrote
 *   0, which readers take as struct).
 * - Anonymous aggregates are deduplicated purely structurally, so two
 *   identical anonymous unions in different CUs become one type.
 */
void
cu_merge_named(struct cunit *cu)
{
	struct itype_queue *cutq = &cu->cu_types;
	struct itype *it, *nit, *prev;

	cu_name = cu->cu_name;

	/* The offset tree has served; the merge trees need the nodes. */
	RB_FOREACH_SAFE(it, ioff_tree, &cu->cu_offt, nit)
		RB_REMOVE(ioff_tree, &cu->cu_offt, it);

	/*
	 * First pass: named struct, union, enum and typedef definitions,
	 * compared by name against the master types.  A definition that
	 * differs from every master of that name becomes an additional
	 * master (the previous tools keep all of them; each CU's
	 * references then resolve to its own definition).  Forward
	 * declarations resolve to the first master.
	 */
	it_cmp_identity = 0;
	TAILQ_FOREACH(it, cutq, it_next) {
		struct itype *alt, *last = NULL, *match = NULL;

		if (it->it_flags & (ITF_FUNC|ITF_OBJ|ITF_DROPPED|ITF_ANON))
			continue;
		if (it->it_type != CTF_K_STRUCT && it->it_type != CTF_K_UNION &&
		    it->it_type != CTF_K_ENUM && it->it_type != CTF_K_TYPEDEF)
			continue;

		stats.st_types_in++;

		prev = RB_FIND(itype_tree, &itypet[it->it_type], it);
		if (prev == NULL) {
			RB_INSERT(itype_tree, &itypet[it->it_type], it);
			continue;
		}

		for (alt = prev; alt != NULL; last = alt, alt = alt->it_alt) {
			if (it->it_flags & ITF_FORWARD) {
				match = alt;
				if (!(alt->it_flags & ITF_FORWARD))
					stats.st_fwd_resolved++;
				break;
			}
			if (alt->it_flags & ITF_FORWARD) {
				/* First definition completes the forward. */
				it_merge(alt, it);
				it->it_flags |= ITF_MERGED;
				stats.st_fwd_resolved++;
				match = alt;
				break;
			}
			if (it_cmp_def(alt, it) == 0) {
				match = alt;
				break;
			}
		}

		if (match == NULL) {
			/*
			 * Conflicting definition: keep it as a new master,
			 * counted and named under -v.
			 */
			stats.st_conflicts++;
			if (verbose > 0)
				warnx("%s %s: definition in %s differs from "
				    "the one in %s",
				    it->it_type == CTF_K_STRUCT ? "struct" :
				    it->it_type == CTF_K_UNION ? "union" :
				    it->it_type == CTF_K_ENUM ? "enum" :
				    "typedef", it_name(it),
				    cu_name != NULL ? cu_name : "?",
				    prev->it_cuname != NULL ? prev->it_cuname :
				    "?");
			last->it_alt = it;
			continue;
		}

		stats.st_types_dup++;
		it->it_flags |= ITF_DROPPED;
		it->it_survivor = match;
	}
	it_cmp_identity = 1;
}

/*
 * Second pass: everything else, compared structurally.  Named
 * aggregates are now all masters, so a reference to one is compared
 * by identity.
 */
void
cu_merge_anon(struct cunit *cu)
{
	struct itype_queue *cutq = &cu->cu_types;
	struct itype *it, *prev;

	cu_name = cu->cu_name;

	TAILQ_FOREACH(it, cutq, it_next) {
		if (it->it_flags & (ITF_FUNC|ITF_OBJ|ITF_DROPPED))
			continue;
		if (!(it->it_flags & ITF_ANON) &&
		    (it->it_type == CTF_K_STRUCT || it->it_type == CTF_K_UNION ||
		    it->it_type == CTF_K_ENUM || it->it_type == CTF_K_TYPEDEF))
			continue;

		stats.st_types_in++;

		prev = RB_FIND(itype_tree, &itypet[it->it_type], it);
		if (prev == NULL) {
			RB_INSERT(itype_tree, &itypet[it->it_type], it);
			continue;
		}

		stats.st_types_dup++;
		if (verbose > 2)
			warnx("dup: kind %u %s idx %u -> %u", it->it_type,
			    it_name(it) != NULL ? it_name(it) : "(anon)",
			    it->it_idx, prev->it_idx);
		it->it_flags |= ITF_DROPPED;
		it->it_survivor = prev;
	}
}

/*
 * Third pass: move symbols to their tree, purge duplicates, map the
 * DIEs.  Unreferenced types are pruned at the end by types_finalize(),
 * once symbols are matched.  Type IDs stay as parsed, unique and
 * increasing; types_finalize() renumbers.
 */
void
cu_merge(struct cunit *cu)
{
	struct itype_queue *cutq = &cu->cu_types;
	struct itype *it, *nit, *first, *ref;

	cu_name = cu->cu_name;

	/* First ``it'' of the unit, once on the global queue. */
	first = TAILQ_FIRST(cutq);
	if (first == NULL)
		return;

	TAILQ_CONCAT(&itypeq, cutq, it_next);

	/*
	 * The offset map, for references back into this unit from later
	 * ones: its transparent DIEs stand for what they resolved to.
	 */
	if (have_ref_addr) {
		TAILQ_FOREACH(it, &cu->cu_aliases, it_next) {
			for (ref = it->it_refp; ref != NULL &&
			    (ref->it_flags & ITF_DROPPED); ref = ref->it_survivor)
				;
			if (ref != NULL)
				iom_add(it->it_off, ref);
		}
	}

	for (it = first; it != NULL; it = nit) {
		nit = TAILQ_NEXT(it, it_next);

		if (!(it->it_flags & ITF_DROPPED)) {
			it->it_flags &= ~ITF_PENDING;
			if (have_ref_addr && it->it_off != 0)
				iom_add(it->it_off, it);
			/*
			 * Local variables only serve as roots for the
			 * types they reference.
			 */
			if (it->it_flags & (ITF_FUNC|ITF_OBJ))
				symb_insert(it);
			continue;
		}

		/* Remove duplicates; nothing points here any more. */
		if (have_ref_addr && it->it_off != 0)
			iom_add(it->it_off, it->it_survivor);
		TAILQ_REMOVE(&itypeq, it, it_next);
		it_free(it);
	}
}

/*
 * Parse a CU: walk its DIEs and build the internal types.
 *
 * DWARF to CTF.  Only units in C (C89 to C23; a unit with no language
 * attribute is taken as C with a warning) are converted, others are
 * skipped and counted.  Type-producing tags are handled wherever they
 * appear; lexical blocks and inlined subroutines are transparent.
 *
 *   base_type              INTEGER or FLOAT: parse_base(), canonical
 *                          integer names so that GCC and clang agree,
 *                          floats classified by size; void is an
 *                          INTEGER of 0 bits, synthesized if absent
 *   pointer_type           POINTER (size = address size)
 *   const/volatile/
 *   restrict_type, typedef CONST, VOLATILE, RESTRICT, TYPEDEF; a
 *                          missing DW_AT_type means void
 *   atomic_type            transparent (CTF has no atomic)
 *   structure/union_type   STRUCT, UNION, or FORWARD when declared only;
 *                          members with bit offsets, bitfields through a
 *                          narrowed clone of their integer type
 *   enumeration_type       ENUM (values truncated to int32, counted) or
 *                          FORWARD
 *   array_type             one ARRAY per subrange, outermost first,
 *                          indexed by a synthetic ``long''; count 0 when
 *                          unknown
 *   subroutine_type        FUNCTION; ``...'' after a named parameter
 *                          makes it variadic (trailing ID 0), alone it
 *                          is a K&R declaration
 *   subprogram             a function symbol candidate: name, return,
 *                          parameters (from the abstract origin for an
 *                          out-of-line inline instance), address;
 *                          declarations are skipped
 *   variable               an object symbol candidate: name, type and
 *                          the address of a DW_OP_addr/addrx location;
 *                          without one it is only matched by name as a
 *                          static-storage variable
 *   anything else          ignored
 *
 * The members, enumerators, subranges and parameters are consumed by
 * their parent's parser; the list order puts them right after it.
 */
void
cu_parse(struct dwcu *dcu, struct cunit *cu)
{
	struct itype_queue *cutq = &cu->cu_types;
	struct ioff_tree *cuot = &cu->cu_offt;
	struct itype *it = NULL;
	struct dwdie *die;
	size_t psz = dcu->dcu_psize;
	size_t off = dcu->dcu_offset;

	assert(RB_EMPTY(cuot));
	assert(TAILQ_EMPTY(&extraq));

	cu_name = NULL;

	/* Only C compilation units are converted. */
	die = STAILQ_FIRST(&dcu->dcu_dies);
	if (die != NULL && die->die_dab->dab_tag == DW_TAG_compile_unit &&
	    !cu_language_ok(die, psz)) {
		stats.st_cus_skipped++;
		return;
	}
	stats.st_cus++;

	STAILQ_FOREACH(die, &dcu->dcu_dies, die_next) {
		uint64_t tag = die->die_dab->dab_tag;

		switch (tag) {
		case DW_TAG_array_type:
			it = parse_array(die, dcu->dcu_psize);
			break;
		case DW_TAG_enumeration_type:
			it = parse_enum(die, dcu->dcu_psize);
			if (it == NULL)
				continue;
			break;
		case DW_TAG_pointer_type:
			it = parse_refers(die, psz, CTF_K_POINTER);
			break;
		case DW_TAG_structure_type:
			it = parse_struct(die, psz, CTF_K_STRUCT, off);
			if (it == NULL)
				continue;
			break;
		case DW_TAG_typedef:
			it = parse_refers(die, psz, CTF_K_TYPEDEF);
			break;
		case DW_TAG_union_type:
			it = parse_struct(die, psz, CTF_K_UNION, off);
			if (it == NULL)
				continue;
			break;
		case DW_TAG_base_type:
			it = parse_base(die, psz);
			if (it == NULL)
				continue;
			break;
		case DW_TAG_const_type:
			it = parse_refers(die, psz, CTF_K_CONST);
			break;
		case DW_TAG_volatile_type:
			it = parse_refers(die, psz, CTF_K_VOLATILE);
			break;
		case DW_TAG_restrict_type:
			it = parse_refers(die, psz, CTF_K_RESTRICT);
			break;
		case DW_TAG_atomic_type:
			parse_alias(die, psz, cu);
			it = NULL;
			continue;
		case DW_TAG_subprogram:
			it = parse_function(die, psz);
			if (it == NULL)
				continue;
			break;
		case DW_TAG_subroutine_type:
			it = parse_funcptr(die, psz);
			break;
		/*
		 * Children are assumed to be right after their parent in
		 * the list.  The parent parsing function takes care of
		 * parsing them.
		 */
		case DW_TAG_member:
		case DW_TAG_enumerator:
		case DW_TAG_subrange_type:
		case DW_TAG_unspecified_parameters:
			continue;
		case DW_TAG_formal_parameter:
			/*
			 * If we skipped the second inline definition,
			 * skip its arguments.
			 */
			if (it == NULL)
				continue;

			/* See comment in subparse_arguments(). */
			if (it->it_type == CTF_K_STRUCT ||
			    it->it_type == CTF_K_UNION ||
			    it->it_type == CTF_K_ENUM ||
			    it->it_type == CTF_K_TYPEDEF)
				continue;

			if (it->it_flags & ITF_OBJ)
				continue;

			/*
			 * A parameter of a function (consumed by its
			 * parser), or of something that is not one in a
			 * malformed file: nothing to do either way.
			 */
			continue;
		case DW_TAG_variable:
			it = parse_variable(die, psz);
			/* Unnamed variables are discarded. */
			if (it == NULL)
				continue;
			break;
#if 1
		case DW_TAG_lexical_block:
		case DW_TAG_inlined_subroutine:
			continue;
#endif
		case DW_TAG_compile_unit:
		default:
			DPRINTF("tag %#" PRIx64 "\n", tag);
			continue;
		}

		TAILQ_INSERT_TAIL(cutq, it, it_next);
		RB_INSERT(ioff_tree, cuot, it);

		/* Synthesized types follow their parent, in ID order. */
		TAILQ_CONCAT(cutq, &extraq, it_next);
	}
}

/*
 * Record the CU name and decide whether its language is C.  A CU
 * without a language attribute is treated as C with a warning.
 */
int
cu_language_ok(struct dwdie *die, size_t psz)
{
	struct dwaval *dav;
	uint64_t lang = 0;
	int seen = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_name:
			cu_name = dav2str(dav);
			break;
		case DW_AT_language:
			lang = dav2val(dav, psz);
			seen = 1;
			break;
		default:
			break;
		}
	}

	if (!seen) {
		warnx("%s: no DW_AT_language, assuming C",
		    cu_name != NULL ? cu_name : "?");
		return 1;
	}

	switch (lang) {
	case DW_LANG_C89:
	case DW_LANG_C:
	case DW_LANG_C99:
	case DW_LANG_C11:
	case DW_LANG_C17:
	case DW_LANG_C23:
		return 1;
	default:
		if (verbose > 1)
			warnx("%s: skipped, DW_AT_language 0x%" PRIx64,
			    cu_name != NULL ? cu_name : "?", lang);
		return 0;
	}
}

/*
 * Canonical spelling of an integer base type name made only of the
 * tokens signed, unsigned, char, short, int and long, so
 * that GCC and clang produce the same container.  Returns NULL when
 * the name contains any other token; otherwise the canonical name and
 * the encoding derived from the tokens.
 *
 * The encoding comes from the tokens, not from DW_AT_encoding: a plain
 * ``char'' is SIGNED CHAR even on an unsigned-char ABI (arm64, powerpc,
 * riscv), where the compiler says DW_ATE_unsigned_char.  That is what
 * the previous tools wrote on those targets too (checked on an arm64
 * object), and the containers are compared against theirs; a target's
 * signedness can be taken from DW_AT_encoding here once that parity no
 * longer matters.
 */
const char *
canon_int_name(const char *name, uint16_t *encp)
{
	static char buf[ITNAME_MAX];
	char tmp[ITNAME_MAX], *tok, *last;
	int is_unsigned = 0, has_char = 0, has_short = 0, nlong = 0;
	size_t n;

	if (name == NULL)
		return NULL;

	n = strlcpy(tmp, name, sizeof(tmp));
	if (n >= sizeof(tmp))
		return NULL;

	for (tok = strtok_r(tmp, " ", &last); tok != NULL;
	    tok = strtok_r(NULL, " ", &last)) {
		if (strcmp(tok, "unsigned") == 0)
			is_unsigned = 1;
		else if (strcmp(tok, "signed") == 0)
			;
		else if (strcmp(tok, "char") == 0)
			has_char = 1;
		else if (strcmp(tok, "short") == 0)
			has_short = 1;
		else if (strcmp(tok, "long") == 0)
			nlong++;
		else if (strcmp(tok, "int") == 0)
			;
		else
			return NULL;
	}

	if (has_char + has_short + (nlong > 0) > 1 || nlong > 2)
		return NULL;

	snprintf(buf, sizeof(buf), "%s%s", is_unsigned ? "unsigned " : "",
	    has_char ? "char" : has_short ? "short" : nlong == 2 ? "long long" :
	    nlong == 1 ? "long" : "int");

	*encp = (is_unsigned ? 0 : CTF_INT_SIGNED) | (has_char ? CTF_INT_CHAR : 0);
	return buf;
}

struct itype *
parse_base(struct dwdie *die, size_t psz)
{
	struct itype *it;
	struct dwaval *dav;
	const char *name = NULL, *cname;
	uint16_t encoding = 0, enc = 0;
	size_t size = 0, ldsz;
	int type;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_name:
			name = dav2str(dav);
			break;
		case DW_AT_encoding:
			enc = dav2val(dav, psz);
			break;
		case DW_AT_byte_size:
			size = dav2val(dav, psz);
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}

	if (name == NULL)
		errx(1, "base type without a name at DIE 0x%zx",
		    die->die_offset);
	if (size > UINT16_MAX / 8)
		errx(1, "base type %s: size %zu too large", name, size);

	/* long double: 12 bytes on 32-bit targets, 16 on 64-bit ones. */
	ldsz = (psz == 4) ? 12 : 16;

	switch (enc) {
	case DW_ATE_unsigned:
	case DW_ATE_address:
		encoding = 0;
		type = CTF_K_INTEGER;
		break;
	case DW_ATE_unsigned_char:
	case DW_ATE_UTF:
		encoding = CTF_INT_CHAR;
		type = CTF_K_INTEGER;
		break;
	case DW_ATE_signed:
		encoding = CTF_INT_SIGNED;
		type = CTF_K_INTEGER;
		break;
	case DW_ATE_signed_char:
		encoding = CTF_INT_SIGNED | CTF_INT_CHAR;
		type = CTF_K_INTEGER;
		break;
	case DW_ATE_boolean:
		encoding = CTF_INT_SIGNED | CTF_INT_BOOL;
		type = CTF_K_INTEGER;
		break;
	case DW_ATE_float:
		if (size == 4)
			encoding = CTF_FP_SINGLE;
		else if (size == 8)
			encoding = CTF_FP_DOUBLE;
		else if (size == ldsz || size == 12 || size == 16)
			encoding = CTF_FP_LDOUBLE;
		else
			goto unsupported;
		type = CTF_K_FLOAT;
		break;
	case DW_ATE_complex_float:
		if (size == 8)
			encoding = CTF_FP_CPLX;
		else if (size == 16)
			encoding = CTF_FP_DCPLX;
		else if (size == 2 * ldsz || size == 24 || size == 32)
			encoding = CTF_FP_LDCPLX;
		else
			goto unsupported;
		type = CTF_K_FLOAT;
		break;
	case DW_ATE_imaginary_float:
		if (size == 4)
			encoding = CTF_FP_IMAGRY;
		else if (size == 8)
			encoding = CTF_FP_DIMAGRY;
		else if (size == ldsz || size == 12 || size == 16)
			encoding = CTF_FP_LDIMAGRY;
		else
			goto unsupported;
		type = CTF_K_FLOAT;
		break;
	default:
	unsupported:
		/*
		 * CTF has no encoding for it (_Float16, __bf16, a future
		 * DW_ATE): no type is made, so what refers to it becomes
		 * void, counted under -v, rather than the whole conversion
		 * failing (compiler-rt's objects in libgcc_s have _Float16).
		 */
		if (verbose)
			warnx("base type %s: DW_AT_encoding 0x%x of size %zu "
			    "has no CTF encoding, made void", name, enc, size);
		return NULL;
	}

	/* Integer names are canonicalized, with the encoding to match. */
	if (type == CTF_K_INTEGER &&
	    (cname = canon_int_name(name, &encoding)) != NULL)
		name = cname;

	it = it_new(++tidx, die->die_offset, name, size, encoding, 0, type, 0);
	it->it_bits = 8 * size;

	return it;
}

struct itype *
parse_refers(struct dwdie *die, size_t psz, int type)
{
	struct itype *it;
	const char *name;
	size_t ref, size;

	refers_attrs(die, psz, &name, &ref, &size);

	if (type == CTF_K_POINTER)
		size = psz;

	it = it_new(++tidx, die->die_offset, name, size, 0, ref, type,
	    ITF_UNRES);

	if (it->it_ref == 0 && (type == CTF_K_CONST ||
	    type == CTF_K_VOLATILE || type == CTF_K_POINTER ||
	    type == CTF_K_TYPEDEF)) {
		/* Work around GCC/clang not emiting a type for void */
		it->it_flags &= ~ITF_UNRES;
		it->it_ref = VOID_OFFSET;
		it->it_refp = void_it;
	}

	return it;
}

/*
 * Remember that the entity at section offset ``off'' has the address
 * ``addr'', found on another DIE of the unit being parsed; applied by
 * cu_resolve().
 */
static void
origin_add(uint64_t off, uint64_t addr)
{
	struct cunit *cu = parsing_cu;

	if (cu->cu_norigins == cu->cu_maxorigins) {
		cu->cu_maxorigins = cu->cu_maxorigins ?
		    2 * cu->cu_maxorigins : 64;
		cu->cu_origins = xreallocarray(cu->cu_origins,
		    cu->cu_maxorigins, sizeof(*cu->cu_origins));
	}
	cu->cu_origins[cu->cu_norigins].io_ref = off;
	cu->cu_origins[cu->cu_norigins].io_addr = addr;
	cu->cu_norigins++;
}

/*
 * The name, DW_AT_type and byte size of a DIE that refers to another
 * type (qualifiers, pointers, typedefs and the like).
 */
static void
refers_attrs(struct dwdie *die, size_t psz, const char **name, size_t *ref,
    size_t *size)
{
	struct dwaval *dav;

	*name = NULL;
	*ref = *size = 0;
	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_name:
			*name = dav2str(dav);
			break;
		case DW_AT_type:
			*ref = dav2val(dav, psz);
			break;
		case DW_AT_byte_size:
			*size = dav2val(dav, psz);
			/* Nonsense on a qualifier or typedef; it_size is 32 bits. */
			if (*size > UINT32_MAX)
				*size = 0;
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}
}

/*
 * A transparent DIE (DW_TAG_atomic_type, ``_Atomic'' in DWARF 5): it
 * stands for its DW_AT_type and is never a CTF type, so it takes no
 * type ID and does not join the CU queue.  It sits in the offset tree
 * so that cu_lookup() sees through it, and in the unit's alias list
 * until the unit is merged and it is freed.
 */
static void
parse_alias(struct dwdie *die, size_t psz, struct cunit *cu)
{
	struct itype *it;
	const char *name;
	size_t ref, size;

	refers_attrs(die, psz, &name, &ref, &size);
	it = it_new(0, die->die_offset, NULL, 0, 0, ref, CTF_K_UNKNOWN,
	    ITF_UNRES | ITF_ALIAS);
	RB_INSERT(ioff_tree, &cu->cu_offt, it);
	TAILQ_INSERT_TAIL(&cu->cu_aliases, it, it_next);
}

struct itype *
parse_array(struct dwdie *die, size_t psz)
{
	struct itype *it;
	struct dwaval *dav;
	const char *name = NULL;
	size_t ref = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_name:
			name = dav2str(dav);
			break;
		case DW_AT_type:
			ref = dav2val(dav, psz);
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}

	it = it_new(++tidx, die->die_offset, name, 0, 0, ref, CTF_K_ARRAY,
	    ITF_UNRES);

	subparse_subrange(die, psz, it);

	return it;
}

struct itype *
parse_enum(struct dwdie *die, size_t psz)
{
	struct itype *it;
	struct dwaval *dav;
	const char *name = NULL;
	size_t size = 0;
	int forward = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_declaration:
			forward = dav2val(dav, psz);
			break;
		case DW_AT_byte_size:
			size = dav2val(dav, psz);
			/* it_size is 32 bits; such a size is a malformed file. */
			if (size > UINT32_MAX)
				return NULL;
			break;
		case DW_AT_name:
			name = dav2str(dav);
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}

	it = it_new(++tidx, die->die_offset, name, size, 0, 0, CTF_K_ENUM,
	    forward ? ITF_FORWARD : 0);

	if (!forward)
		subparse_enumerator(die, psz, it);

	return it;
}

void
subparse_subrange(struct dwdie *die, size_t psz, struct itype *it)
{
	struct dwaval *dav;
	struct itype *inner;
	int first = 1;

	assert(it->it_type == CTF_K_ARRAY);

	if (die->die_dab->dab_children == DW_CHILDREN_no)
		return;

	/*
	 * This loop assumes that the children of a DIE are just
	 * after it on the list.
	 */
	/*
	 * Each dimension is one ARRAY type, the outermost first.
	 * The DIE's own type is the outermost; inner
	 * dimensions are synthesized and the element type reference
	 * moves to the innermost.
	 */
	while ((die = STAILQ_NEXT(die, die_next)) != NULL) {
		uint64_t tag = die->die_dab->dab_tag;
		uint64_t nelems = 0, val;
		int constant;

		if (tag != DW_TAG_subrange_type)
			break;

		STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
			/* Only constant bounds are usable; a VLA has 0. */
			switch (dav->dav_dat->dat_form) {
			case DW_FORM_data1:
			case DW_FORM_data2:
			case DW_FORM_data4:
			case DW_FORM_data8:
			case DW_FORM_sdata:
			case DW_FORM_udata:
			case DW_FORM_implicit_const:
				constant = 1;
				break;
			default:
				constant = 0;
				break;
			}
			if (!constant)
				continue;

			val = dav2val(dav, psz);
			switch (dav->dav_dat->dat_attr) {
			case DW_AT_count:
				nelems = val;
				break;
			case DW_AT_upper_bound:
				/*
				 * A signed -1 means an empty array.  GCC
				 * writes that -1 in the unsigned form of the
				 * target's size type: all ones in data4 on a
				 * 32-bit target, in data8 on a 64-bit one.
				 */
				if (((dav->dav_dat->dat_form == DW_FORM_sdata ||
				    dav->dav_dat->dat_form ==
				    DW_FORM_implicit_const) &&
				    (int64_t)val < 0) ||
				    (dav->dav_dat->dat_form == DW_FORM_data4 &&
				    psz == 4 && val == 0xffffffff) ||
				    (dav->dav_dat->dat_form == DW_FORM_data8 &&
				    val == UINT64_MAX))
					nelems = 0;
				else
					nelems = val + 1;
				break;
			default:
				DPRINTF("attribute %#" PRIx64 "\n",
				    dav->dav_dat->dat_attr);
				break;
			}
		}

		if (nelems >= UINT32_MAX)
			errx(1, "array at DIE 0x%zx: %" PRIu64 " elements",
			    die->die_offset, nelems);

		if (first) {
			it->it_nelems = nelems;
			first = 0;
			continue;
		}

		inner = it_new(++tidx, 0, NULL, 0, 0, it->it_ref, CTF_K_ARRAY,
		    ITF_UNRES);
		inner->it_nelems = nelems;
		TAILQ_INSERT_TAIL(&extraq, inner, it_next);

		it->it_flags &= ~ITF_UNRES;
		it->it_ref = 0;
		it->it_refp = inner;
		it = inner;
	}
}

void
subparse_enumerator(struct dwdie *die, size_t psz, struct itype *it)
{
	struct imember *im;
	struct dwaval *dav;

	assert(it->it_type == CTF_K_ENUM);

	if (die->die_dab->dab_children == DW_CHILDREN_no)
		return;

	/*
	 * This loop assumes that the children of a DIE are just
	 * after it on the list.
	 */
	while ((die = STAILQ_NEXT(die, die_next)) != NULL) {
		uint64_t tag = die->die_dab->dab_tag;
		uint64_t val = 0;
		const char *name = NULL;

		if (tag != DW_TAG_enumerator)
			break;

		STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
			switch (dav->dav_dat->dat_attr) {
			case DW_AT_name:
				name = dav2str(dav);
				break;
			case DW_AT_const_value:
				val = dav2val(dav, psz);
				break;
			default:
				DPRINTF("attribute %#" PRIx64 "\n",
				    dav->dav_dat->dat_attr);
				break;
			}
		}

		if (name == NULL) {
			warnx("enum %s with anon member",
			    it_name(it) != NULL ? it_name(it) : "(anon)");
			continue;
		}

		/* Values are stored as int32 by truncation, as before. */
		if ((int64_t)(int32_t)val != (int64_t)val) {
			stats.st_enum_trunc++;
			if (verbose > 1)
				warnx("enum %s: value of %s does not fit "
				    "in 32 bits", it_name(it) != NULL ?
				    it_name(it) : "(anon)", name);
		}

		im = im_new(name, val, 0);
		assert(it->it_nelems < UINT_MAX);
		it->it_nelems++;
		TAILQ_INSERT_TAIL(&it->it_members, im, im_next);
	}
}

struct itype *
parse_struct(struct dwdie *die, size_t psz, int type, size_t off)
{
	struct itype *it = NULL;
	struct dwaval *dav;
	const char *name = NULL;
	unsigned int flags = 0;
	size_t size = 0;
	int forward = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_declaration:
			forward = dav2val(dav, psz);
			break;
		case DW_AT_byte_size:
			size = dav2val(dav, psz);
			/* it_size is 32 bits; such a size is a malformed file. */
			if (size > UINT32_MAX)
				return NULL;
			break;
		case DW_AT_name:
			name = dav2str(dav);
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}


	if (forward)
		flags = ITF_FORWARD;
	it = it_new(++tidx, die->die_offset, name, size, 0, 0, type, flags);
	subparse_member(die, psz, it, off);

	return it;
}

void
subparse_member(struct dwdie *die, size_t psz, struct itype *it, size_t offset)
{
	struct imember *im;
	struct dwaval *dav;
	const char *name;
	uint64_t off, ref, bits, bitoff, databitoff, membytes;
	int has_bitoff, has_databitoff;
	uint8_t lvl = die->die_lvl;

	(void)offset;
	assert(it->it_type == CTF_K_STRUCT || it->it_type == CTF_K_UNION);

	if (die->die_dab->dab_children == DW_CHILDREN_no)
		return;

	/*
	 * This loop assumes that the children of a DIE are just
	 * after it on the list.
	 */
	while ((die = STAILQ_NEXT(die, die_next)) != NULL) {
		int64_t tag = die->die_dab->dab_tag;

		if (die->die_lvl <= lvl)
			break;

		/* Skip members of members */
		if (die->die_lvl > lvl + 1)
			continue;
		/*
		 * Nested declaration.
		 *
		 * This matches the case where a ``struct'', ``union'',
		 * ``enum'' or ``typedef'' is first declared "inside" a
		 * union or struct declaration.  Anything that is not a
		 * member is handled by cu_parse().
		 */
		if (tag != DW_TAG_member)
			continue;

		it->it_flags |= ITF_UNRES_MEMB;

		name = NULL;
		off = ref = bits = bitoff = databitoff = membytes = 0;
		has_bitoff = has_databitoff = 0;

		STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
			switch (dav->dav_dat->dat_attr) {
			case DW_AT_name:
				name = dav2str(dav);
				break;
			case DW_AT_type:
				ref = dav2val(dav, psz);
				break;
			case DW_AT_data_member_location:
				off = 8 * dav2val(dav, psz);
				break;
			case DW_AT_byte_size:
				membytes = dav2val(dav, psz);
				break;
			case DW_AT_bit_size:
				bits = dav2val(dav, psz);
				break;
			case DW_AT_bit_offset:
				bitoff = dav2val(dav, psz);
				has_bitoff = 1;
				break;
			case DW_AT_data_bit_offset:
				databitoff = dav2val(dav, psz);
				has_databitoff = 1;
				break;
			default:
				DPRINTF("attribute %#" PRIx64 "\n",
				    dav->dav_dat->dat_attr);
				break;
			}
		}

		if (bits >= UINT16_MAX)
			errx(1, "%s: member %s: bit size %" PRIu64 " too large",
			    it_name(it) != NULL ? it_name(it) : "(anon)",
			    name != NULL ? name : "(anon)", bits);

		/*
		 * Bitfield position.  DWARF 4 gives the
		 * offset from the start of the aggregate directly;
		 * DWARF 2/3 count from the most significant bit of the
		 * storage unit, which depends on the target byte order.
		 * Without a member byte size the storage unit is the
		 * member's base type, known once types are resolved.
		 */
		if (has_databitoff)
			off += databitoff;
		else if (has_bitoff && !target_bigendian && membytes != 0)
			off += 8 * membytes - bitoff - bits;
		else if (has_bitoff && target_bigendian)
			off += bitoff;

		im = im_new(name, ref, off);
		im->im_bits = bits;
		if (has_bitoff && !target_bigendian && membytes == 0) {
			im->im_bitoff = bitoff;
			im->im_flags |= IMF_BITOFF;
		}
		assert(it->it_nelems < UINT_MAX);
		it->it_nelems++;
		TAILQ_INSERT_TAIL(&it->it_members, im, im_next);
	}
}


void
subparse_arguments(struct dwdie *die, size_t psz, struct itype *it)
{
	struct imember *im;
	struct dwaval *dav;
	size_t ref = 0;
	uint8_t lvl = die->die_lvl;

	assert(it->it_type == CTF_K_FUNCTION);

	if (die->die_dab->dab_children == DW_CHILDREN_no)
		return;

	/*
	 * This loop assumes that the children of a DIE are after it
	 * on the list.  Besides parameters, a function has local
	 * variables, lexical blocks, labels, inlined calls and nested
	 * type declarations as children; those (and their own children)
	 * are handled by cu_parse() and skipped here.
	 */
	while ((die = STAILQ_NEXT(die, die_next)) != NULL) {
		uint64_t tag = die->die_dab->dab_tag;

		if (die->die_lvl <= lvl)
			break;
		if (die->die_lvl > lvl + 1)
			continue;

		/*
		 * ``...'' makes the type variadic only when a named
		 * parameter precedes it; alone it is a K&R declaration.
		 */
		if (tag == DW_TAG_unspecified_parameters) {
			if (it->it_nelems > 0)
				it->it_flags |= ITF_VARARGS;
			continue;
		}

		if (tag != DW_TAG_formal_parameter)
			continue;

		it->it_flags |= ITF_UNRES_MEMB;

		ref = 0;
		STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
			switch (dav->dav_dat->dat_attr) {
			case DW_AT_type:
				ref = dav2val(dav, psz);
				break;
			default:
				DPRINTF("attribute %#" PRIx64 "\n",
				    dav->dav_dat->dat_attr);
				break;
			}
		}

		im = im_new(NULL, ref, 0);
		assert(it->it_nelems < UINT_MAX);
		it->it_nelems++;
		TAILQ_INSERT_TAIL(&it->it_members, im, im_next);
	}
}

struct itype *
parse_function(struct dwdie *die, size_t psz)
{
	struct itype *it;
	struct dwaval *dav;
	const char *name = NULL;
	size_t ref = 0, origin = 0;
	uint64_t addr = 0;
	unsigned int flags = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_name:
			name = dav2str(dav);
			break;
		case DW_AT_type:
			ref = dav2val(dav, psz);
			break;
		case DW_AT_low_pc:
			if (dav->dav_dat->dat_form == DW_FORM_addr) {
				addr = dav2val(dav, psz);
				flags |= ITF_ADDR;
			} else if (dav_is_addrx(dav) &&
			    cu_addrx(dav->dav_u64, psz, &addr) == 0)
				flags |= ITF_ADDR;
			break;
		case DW_AT_abstract_origin:
			origin = dav2val(dav, psz);
			break;
		case DW_AT_external:
			if (dav2val(dav, psz))
				flags |= ITF_EXTERN;
			break;
		case DW_AT_declaration:
			/* A prototype, not a function. */
			if (dav2val(dav, psz))
				return NULL;
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}

	/*
	 * Concrete out-of-line instance of an inline function: name and
	 * type live in the abstract DIE, which gets the address.
	 */
	if (origin != 0) {
		if (flags & ITF_ADDR)
			origin_add(origin + cu_off, addr);
		return NULL;
	}

	/*
	 * Work around for clang 4.0 generating DW_TAG_subprogram without
	 * any attribute.
	 */
	if (name == NULL)
		return NULL;

	it = it_new(++fidx, die->die_offset, name, 0, 0, ref, CTF_K_FUNCTION,
	    ITF_UNRES|ITF_FUNC|flags);
	it->it_addr = addr;

	subparse_arguments(die, psz, it);

	if (it->it_ref == 0) {
		/* Work around GCC not emiting a type for void */
		it->it_flags &= ~ITF_UNRES;
		it->it_ref = VOID_OFFSET;
		it->it_refp = void_it;
	}

	return it;
}

struct itype *
parse_funcptr(struct dwdie *die, size_t psz)
{
	struct itype *it;
	struct dwaval *dav;
	const char *name = NULL;
	size_t ref = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_name:
			name = dav2str(dav);
			break;
		case DW_AT_type:
			ref = dav2val(dav, psz);
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}

	it = it_new(++tidx, die->die_offset, name, 0, 0, ref, CTF_K_FUNCTION,
	    ITF_UNRES);

	subparse_arguments(die, psz, it);

	if (it->it_ref == 0) {
		/* Work around GCC not emiting a type for void */
		it->it_flags &= ~ITF_UNRES;
		it->it_ref = VOID_OFFSET;
		it->it_refp = void_it;
	}

	return it;
}

/*
 * Variables.  Only a named variable whose location is a
 * plain address can be a symbol; the others (locals, declarations,
 * optimized away) are kept so that cu_parse() can skip their children
 * but never become roots, see types_finalize().
 */
struct itype *
parse_variable(struct dwdie *die, size_t psz)
{
	struct itype *it = NULL;
	struct dwaval *dav;
	const char *name = NULL;
	size_t ref = 0, spec = 0;
	uint64_t addr = 0;
	unsigned int flags = 0;
	int forward = 0, global = 0;

	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_declaration:
			forward = dav2val(dav, psz);
			break;
		case DW_AT_specification:
			spec = dav2val(dav, psz);
			break;
		case DW_AT_name:
			name = dav2str(dav);
			break;
		case DW_AT_type:
			ref = dav2val(dav, psz);
			break;
		case DW_AT_external:
			if (dav2val(dav, psz))
				flags |= ITF_EXTERN;
			break;
		case DW_AT_location:
			switch (dav->dav_dat->dat_form) {
			case DW_FORM_block:
			case DW_FORM_block1:
			case DW_FORM_block2:
			case DW_FORM_block4:
			case DW_FORM_exprloc:
				if (dav->dav_buf.len >= 1 + psz &&
				    (uint8_t)dav->dav_buf.buf[0] == DW_OP_addr) {
					global = 1;
					addr = dw_addr(dav->dav_buf.buf + 1,
					    psz);
					flags |= ITF_ADDR;
				} else if (dav->dav_buf.len >= 2 &&
				    (uint8_t)dav->dav_buf.buf[0] == DW_OP_addrx) {
					/* DWARF 5: an index into .debug_addr. */
					struct dwbuf b = {
						.buf = dav->dav_buf.buf + 1,
						.len = dav->dav_buf.len - 1
					};
					uint64_t idx;

					if (dw_read_uleb128(&b, &idx) == 0 &&
					    cu_addrx(idx, psz, &addr) == 0) {
						global = 1;
						flags |= ITF_ADDR;
					}
				}
				break;
			default:
				break;
			}
			break;
		default:
			DPRINTF("attribute %#" PRIx64 "\n", dav->dav_dat->dat_attr);
			break;
		}
	}

	/*
	 * GCC gives a global declared earlier (extern in a header) a
	 * defining DIE that carries only DW_AT_specification and the
	 * location; name and type stay on the declaration, which gets
	 * the address through the origin table like an out-of-line
	 * inline function.  (clang repeats name and type instead.)
	 */
	if (spec != 0) {
		if (global && (flags & ITF_ADDR))
			origin_add(spec + cu_off, addr);
		return NULL;
	}

	if (ref == 0 || name == NULL)
		return NULL;

	if (!global || forward)
		flags = (flags & ~ITF_ADDR) | ITF_LOCAL;

	it = it_new(++oidx, die->die_offset, name, 0, 0, ref, 0,
	    ITF_UNRES|ITF_OBJ|flags);
	it->it_addr = addr;

	return it;
}

size_t
dav2val(struct dwaval *dav, size_t psz)
{
	uint64_t val = (uint64_t)-1;

	switch (dav->dav_dat->dat_form) {
	case DW_FORM_addr:
		if (psz == sizeof(uint32_t))
			val = dav->dav_u32;
		else
			val = dav->dav_u64;
		break;
	case DW_FORM_ref_addr:
		/*
		 * Relative to the section, where every other reference is
		 * relative to the unit: made unit-relative like them (it
		 * may wrap), so that adding the unit's offset back gives
		 * the section offset, which cu_lookup() finds in whichever
		 * unit holds it.
		 */
		val = dav->dav_u64 - cu_off;
		break;
	case DW_FORM_block1:
	case DW_FORM_block2:
	case DW_FORM_block4:
	case DW_FORM_block:
	case DW_FORM_exprloc:
		dw_loc_parse(&dav->dav_buf, NULL, &val, NULL);
		break;
	case DW_FORM_flag:
	case DW_FORM_data1:
	case DW_FORM_ref1:
		val = dav->dav_u8;
		break;
	case DW_FORM_data2:
	case DW_FORM_ref2:
		val = dav->dav_u16;
		break;
	case DW_FORM_data4:
	case DW_FORM_ref4:
	case DW_FORM_sec_offset:
		val = dav->dav_u32;
		break;
	case DW_FORM_sdata:
	case DW_FORM_data8:
	case DW_FORM_ref8:
	case DW_FORM_udata:
	case DW_FORM_ref_udata:
		val = dav->dav_u64;
		break;
	case DW_FORM_strp:
	case DW_FORM_line_strp:
		val = dav->dav_u32;
		break;
	case DW_FORM_flag_present:
		val = 1;
		break;
	/* DWARF 5: indices, resolved by cu_strx() and cu_addrx(). */
	case DW_FORM_strx:
	case DW_FORM_strx1:
	case DW_FORM_strx2:
	case DW_FORM_strx3:
	case DW_FORM_strx4:
	case DW_FORM_addrx:
	case DW_FORM_addrx1:
	case DW_FORM_addrx2:
	case DW_FORM_addrx3:
	case DW_FORM_addrx4:
	case DW_FORM_loclistx:
	case DW_FORM_rnglistx:
		val = dav->dav_u64;
		break;
	case DW_FORM_implicit_const:
		val = (uint64_t)dav->dav_s64;
		break;
	default:
		break;
	}

	return val;
}

const char *
dav2str(struct dwaval *dav)
{
	const char *str = NULL;

	switch (dav->dav_dat->dat_form) {
	case DW_FORM_string:
		str = dav->dav_str;
		break;
	case DW_FORM_strp:
		str = dstr_at(dav->dav_u32);
		break;
	case DW_FORM_strx:
	case DW_FORM_strx1:
	case DW_FORM_strx2:
	case DW_FORM_strx3:
	case DW_FORM_strx4:
		str = cu_strx(dav->dav_u64);
		break;
	default:
		/* DW_FORM_line_strp (.debug_line_str) is not loaded. */
		break;
	}

	return str;
}

/*
 * DW_AT_str_offsets_base and DW_AT_addr_base of the unit DIE; zero when
 * absent, as in every unit before DWARF 5.
 */
static void
cu_bases(struct dwcu *dcu)
{
	struct dwdie *die = STAILQ_FIRST(&dcu->dcu_dies);
	struct dwaval *dav;

	cu_off = dcu->dcu_offset;
	cu_stroff_base = cu_addr_base = 0;
	if (die == NULL)
		return;
	STAILQ_FOREACH(dav, &die->die_avals, dav_next) {
		switch (dav->dav_dat->dat_attr) {
		case DW_AT_str_offsets_base:
			cu_stroff_base = dav2val(dav, dcu->dcu_psize);
			break;
		case DW_AT_addr_base:
			cu_addr_base = dav2val(dav, dcu->dcu_psize);
			break;
		default:
			break;
		}
	}
}

/* Address number ``idx'' of the unit, from .debug_addr. */
static int
cu_addrx(uint64_t idx, size_t psz, uint64_t *addr)
{
	uint64_t off;

	if (daddrbuf == NULL || psz == 0 || cu_addr_base > daddrlen ||
	    idx > (daddrlen - cu_addr_base) / psz)
		return -1;
	off = cu_addr_base + idx * psz;
	if (off + psz > daddrlen)
		return -1;
	*addr = dw_addr(daddrbuf + off, psz);
	return 0;
}

/* String number ``idx'' of the unit, through .debug_str_offsets. */
static const char *
cu_strx(uint64_t idx)
{
	uint64_t off, soff;

	/* 32-bit DWARF: 4-byte entries. */
	if (dstroffbuf == NULL || cu_stroff_base > dstrofflen ||
	    idx > (dstrofflen - cu_stroff_base) / 4)
		return NULL;
	off = cu_stroff_base + idx * 4;
	if (off + 4 > dstrofflen)
		return NULL;
	soff = dw_addr(dstroffbuf + off, 4);
	return dstr_at(soff);
}

/*
 * The string at ``off'' in .debug_str, NULL when the offset is outside
 * the section or no terminator follows it (a truncated section).
 */
static const char *
dstr_at(uint64_t off)
{
	if (dstrbuf == NULL || off >= dstrlen ||
	    memchr(dstrbuf + off, '\0', dstrlen - off) == NULL)
		return NULL;
	return dstrbuf + off;
}

static int
dav_is_addrx(struct dwaval *dav)
{
	switch (dav->dav_dat->dat_form) {
	case DW_FORM_addrx:
	case DW_FORM_addrx1:
	case DW_FORM_addrx2:
	case DW_FORM_addrx3:
	case DW_FORM_addrx4:
		return 1;
	default:
		return 0;
	}
}
