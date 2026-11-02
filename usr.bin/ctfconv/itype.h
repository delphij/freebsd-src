/*	$OpenBSD: itype.h,v 1.5 2019/11/11 19:10:35 mpi Exp $ */

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

#ifndef _ITTYPE_H_
#define _ITTYPE_H_

#include <sys/cdefs.h>
#include <sys/queue.h>

#define ITNAME_MAX	128

struct imember;

/*
 * Internal type representation.
 *
 * Some bits of DWARF that we want to keep around to resolve types and
 * variables to their intrinsics.
 */
struct itype {
	TAILQ_ENTRY(itype)	 it_next;   /* itype: global queue of types */
	TAILQ_ENTRY(itype)	 it_symb;   /* itype: global queue of symbol */
	RB_ENTRY(itype)		 it_node;   /* itype: per-type tree of types */
	RB_ENTRY(itype)		 it_anode;  /* itype: tree of symbol addresses */

	TAILQ_HEAD(, imember)	 it_members;/* itype: members of struct/union */

	size_t			 it_off;    /* DWARF: matching .abbrev offset */
	uint64_t		 it_ref;    /* DWARF: CU offset of ref. type */
	uint64_t		 it_addr;   /* DWARF: address, symbols only */

	struct itype		*it_refp;   /* itype: resolved type */
	struct itype		*it_survivor;/* itype: duplicate kept instead */
	struct itype		*it_alt;    /* itype: next same-name definition */
	struct itype		*it_addrnext;/* itype: next symbol at same address */
	const char		*it_cuname; /* DWARF: CU name, symbols only */

	char			 it_name[ITNAME_MAX];/* CTF: type name */
	uint32_t		 it_size;   /* CTF: size in bytes */
	uint32_t		 it_bits;   /* CTF: encoding width (INTEGER/FLOAT) */
	uint32_t		 it_nelems; /* CTF: # of members or arguments */
	uint32_t		 it_idx;    /* CTF: generated type ID */
	uint16_t		 it_enc;    /* CTF: base type encoding */
	uint16_t		 it_type;   /* CTF: type */

	unsigned int		 it_flags;  /* itype: parser flags */
#define	ITF_UNRES		 0x001	    /* needs to be resolved */
#define	ITF_UNRES_MEMB		 0x002	    /* members need to be resolved */
#define	ITF_FUNC		 0x004	    /* is a function */
#define	ITF_OBJ			 0x008	    /* is an object */
#define	ITF_FORWARD		 0x010	    /* is a forward declaration */
#define	ITF_INSERTED		 0x020	    /* already found/inserted */
#define	ITF_USED		 0x040	    /* referenced in the current CU */
#define	ITF_ANON		 0x080	    /* type without name */
#define	ITF_VARARGS		 0x100	    /* function (type) is variadic */
#define	ITF_DROPPED		 0x200	    /* duplicate, about to be freed */
#define	ITF_ROOT		 0x400	    /* CTF isroot hint */
#define	ITF_MERGED		 0x800	    /* definition moved into a forward */
#define	ITF_LOCAL		0x1000	    /* variable that is not a symbol */
#define	ITF_EXTERN		0x2000	    /* DW_AT_external */
#define	ITF_ADDR		0x4000	    /* it_addr is valid */
#define	ITF_ALIAS		0x8000	    /* transparent DIE, see cu_lookup() */
#define	ITF_VISITL		0x10000	    /* on the left of an it_cmp_def() path */
#define	ITF_VISITR		0x20000	    /* on the right of one */
#define	ITF_PENDING		0x40000	    /* parsed, its unit not merged yet */
#define	ITF_MASK		(ITF_INSERTED|ITF_USED)
};

/*
 * Member for types with a variable length (struct, array, etc).
 */
struct imember {
	TAILQ_ENTRY(imember)	 im_next;
	char			 im_name[ITNAME_MAX]; /* struct field name */
	size_t			 im_ref;    /* CU offset of the field type */
	size_t			 im_off;    /* field offset in struct/union */
	uint32_t		 im_bits;   /* DW_AT_bit_size, 0 if absent */
	uint32_t		 im_bitoff; /* DW_AT_bit_offset, if IMF_BITOFF */
	struct itype		*im_refp;   /* resolved CTF type */
	unsigned int		 im_flags;  /* parser flags */
#define	IMF_ANON		 0x01	    /* member without name */
#define	IMF_BITOFF		 0x02	    /* im_off needs the storage size */
};

TAILQ_HEAD(itype_queue, itype);
RB_HEAD(isymb_tree, itype);
RB_HEAD(iaddr_tree, itype);

/* lists of types, functions & data objects */
extern struct itype_queue itypeq, ifuncq, iobjq;
extern struct isymb_tree isymbt;	    /* tree of symbols, by name */
extern struct iaddr_tree iaddrt;	    /* tree of symbols, by address */
extern uint32_t tidx;		    	    /* type index */
extern struct itype *long_it;		    /* array index type, "long" */

/* Target properties and run-time options, set by ctfconv.c. */
extern int ctf_version;			    /* 2 or 3 */
extern int target_bigendian;		    /* ELFDATA2MSB input */
extern int target_reloc;		    /* ET_REL input */
extern unsigned int target_psz;		    /* target address size, bytes */
extern int verbose;			    /* -v count */
extern const char *dstrbuf;		    /* .debug_str contents */
extern size_t dstrlen;
extern const char *daddrbuf;		    /* .debug_addr (DWARF 5) */
extern size_t daddrlen;
extern const char *dstroffbuf;		    /* .debug_str_offsets (DWARF 5) */
extern size_t dstrofflen;

/* Counters reported under -v. */
struct ctfstats {
	unsigned long	 st_cus;	    /* CUs converted */
	unsigned long	 st_cus_skipped;    /* CUs skipped (language, unit type) */
	unsigned long	 st_types_in;	    /* types parsed from DWARF */
	unsigned long	 st_types_dup;	    /* merged as duplicates */
	unsigned long	 st_types_pruned;   /* reachable only via dropped */
	unsigned long	 st_conflicts;	    /* named definitions dropped */
	unsigned long	 st_fwd_resolved;   /* forwards completed */
	unsigned long	 st_fwd_left;	    /* forwards emitted */
	unsigned long	 st_bitfields;	    /* bitfield clones created */
	unsigned long	 st_enum_trunc;	    /* enumerators outside int32 */
	unsigned long	 st_sym_typed;	    /* symbols with a type */
	unsigned long	 st_sym_byaddr;	    /* ... matched by address */
	unsigned long	 st_sym_byfile;	    /* ... matched by name and file */
	unsigned long	 st_sym_byname;	    /* ... matched by name alone */
	unsigned long	 st_sym_untyped;    /* placeholder entries */
	unsigned long	 st_unres;	    /* references to no DIE, made void */
	unsigned long	 st_xrefs;	    /* references into other units */
	unsigned long	 st_types_out;	    /* types written */
};
extern struct ctfstats stats;

RB_PROTOTYPE(isymb_tree, itype, it_node, it_name_cmp);
RB_PROTOTYPE(iaddr_tree, itype, it_anode, it_addr_cmp);

struct itype *it_dup(struct itype *);
const char *it_name(struct itype *);
const char *im_name(struct imember *);

/* parse.c */
int dwarf_parse(const char *, size_t, const char *, size_t);
void types_finalize(void);

/* generate.c */
int generate(int, const char *, const char *, const char *, int);
int generate_data(const char *, int, char **, size_t *);

#endif /*_ITTYPE_H_ */
