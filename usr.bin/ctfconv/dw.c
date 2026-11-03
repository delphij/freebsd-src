/*	$OpenBSD: dw.c,v 1.6 2024/02/21 13:16:14 claudio Exp $ */

/*
 * Copyright (c) 2016 Martin Pieuchot
 * Copyright (c) 2014 Matthew Dempsky <matthew@dempsky.org>
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

#include <sys/queue.h>

#include <dwarf.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dw.h"
#include "pool.h"
#include "word.h"

#ifndef NOPOOL
static struct pool dcu_pool, die_pool, dav_pool, dab_pool, dat_pool;
#endif /* NOPOOL */

static int	 dw_read_u8(struct dwbuf *, uint8_t *);
static int	 dw_read_u16(struct dwbuf *, uint16_t *);
static int	 dw_read_u32(struct dwbuf *, uint32_t *);
static int	 dw_read_u64(struct dwbuf *, uint64_t *);

static int	 dw_read_sleb128(struct dwbuf *, int64_t *);
static int	 dw_read_uleb128(struct dwbuf *, uint64_t *);

static int	 dw_read_bytes(struct dwbuf *, void *, size_t);
static int	 dw_read_string(struct dwbuf *, const char **);
static int	 dw_read_buf(struct dwbuf *, struct dwbuf *, size_t);

static int	 dw_skip_bytes(struct dwbuf *, size_t);

static int	 dw_attr_parse(struct dwbuf *, struct dwattr *, uint8_t,
		     struct dwaval_queue *);
static void	 dw_attr_purge(struct dwaval_queue *);
static int	 dw_die_parse(struct dwbuf *, size_t, uint8_t,
		     struct dwabbrev_queue *, struct dwdie_queue *);
static void	 dw_die_purge(struct dwdie_queue *);

static int
dw_read_bytes(struct dwbuf *d, void *v, size_t n)
{
	if (d->len < n)
		return -1;
	memcpy(v, d->buf, n);
	d->buf += n;
	d->len -= n;
	return 0;
}

static int
dw_read_u8(struct dwbuf *d, uint8_t *v)
{
	return dw_read_bytes(d, v, sizeof(*v));
}

/*
 * Multi-byte DWARF fields are in the target's byte order; swap them
 * when it differs from the host's.
 */
int	dw_bigendian;

uint64_t
dw_addr(const char *p, size_t size)
{
	return load_word(p, size, dw_bigendian);
}

static int
dw_read_u16(struct dwbuf *d, uint16_t *v)
{
	if (dw_read_bytes(d, v, sizeof(*v)))
		return -1;
	if (dw_bigendian != host_bigendian())
		*v = (uint16_t)((*v >> 8) | (*v << 8));
	return 0;
}

static int
dw_read_u32(struct dwbuf *d, uint32_t *v)
{
	if (dw_read_bytes(d, v, sizeof(*v)))
		return -1;
	if (dw_bigendian != host_bigendian())
		*v = ((*v >> 24) & 0xff) | ((*v >> 8) & 0xff00) |
		    ((*v << 8) & 0xff0000) | (*v << 24);
	return 0;
}

static int
dw_read_u64(struct dwbuf *d, uint64_t *v)
{
	if (dw_read_bytes(d, v, sizeof(*v)))
		return -1;
	if (dw_bigendian != host_bigendian())
		*v = ((uint64_t)dw_addr((const char *)v, 8));
	return 0;
}

/* Read a DWARF LEB128 (little-endian base-128) value. */
static inline int
dw_read_leb128(struct dwbuf *d, uint64_t *v, int signextend)
{
	unsigned int shift = 0;
	uint64_t res = 0;
	uint8_t x;

	while (shift < 64 && !dw_read_u8(d, &x)) {
		res |= (uint64_t)(x & 0x7f) << shift;
		shift += 7;
		if ((x & 0x80) == 0) {
			if (signextend && shift < 64 && (x & 0x40) != 0)
				res |= ~(uint64_t)0 << shift;
			*v = res;
			return 0;
		}
	}
	return -1;
}

static int
dw_read_sleb128(struct dwbuf *d, int64_t *v)
{
	return dw_read_leb128(d, (uint64_t *)v, 1);
}

static int
dw_read_uleb128(struct dwbuf *d, uint64_t *v)
{
	return dw_read_leb128(d, v, 0);
}

/* Read a NUL terminated string. */
static int
dw_read_string(struct dwbuf *d, const char **s)
{
	const char *end = memchr(d->buf, '\0', d->len);
	size_t n;

	if (end == NULL)
		return -1;

	n = end - d->buf + 1;
	*s = d->buf;
	d->buf += n;
	d->len -= n;
	return 0;
}

static int
dw_read_buf(struct dwbuf *d, struct dwbuf *v, size_t n)
{
	if (d->len < n)
		return -1;
	v->buf = d->buf;
	v->len = n;
	d->buf += n;
	d->len -= n;
	return 0;
}

static int
dw_skip_bytes(struct dwbuf *d, size_t n)
{
	if (d->len < n)
		return -1;
	d->buf += n;
	d->len -= n;
	return 0;
}

static int
dw_attr_parse(struct dwbuf *dwbuf, struct dwattr *dat, uint8_t psz,
    struct dwaval_queue *davq)
{
	struct dwaval	*dav;
	uint64_t	 form = dat->dat_form;
	int		 error = 0, i = 0;

	while (form == DW_FORM_indirect) {
		/* XXX loop prevention not strict enough? */
		if (dw_read_uleb128(dwbuf, &form) || (++i > 3))
			return ELOOP;
	}

	dav = pzalloc(&dav_pool, sizeof(*dav));
	if (dav == NULL)
		return ENOMEM;

	dav->dav_dat = dat;

	switch (form) {
	case DW_FORM_addr:
	case DW_FORM_ref_addr:
		if (psz == sizeof(uint32_t))
			error = dw_read_u32(dwbuf, &dav->dav_u32);
		else
			error = dw_read_u64(dwbuf, &dav->dav_u64);
		break;
	case DW_FORM_block1:
		error = dw_read_u8(dwbuf, &dav->dav_u8);
		if (error == 0)
			error = dw_read_buf(dwbuf, &dav->dav_buf, dav->dav_u8);
		break;
	case DW_FORM_block2:
		error = dw_read_u16(dwbuf, &dav->dav_u16);
		if (error == 0)
			error = dw_read_buf(dwbuf, &dav->dav_buf, dav->dav_u16);
		break;
	case DW_FORM_block4:
		error = dw_read_u32(dwbuf, &dav->dav_u32);
		if (error == 0)
			error = dw_read_buf(dwbuf, &dav->dav_buf, dav->dav_u32);
		break;
	case DW_FORM_block:
		error = dw_read_uleb128(dwbuf, &dav->dav_u64);
		if (error == 0)
			error = dw_read_buf(dwbuf, &dav->dav_buf, dav->dav_u64);
		break;
	case DW_FORM_data1:
	case DW_FORM_flag:
	case DW_FORM_ref1:
		error = dw_read_u8(dwbuf, &dav->dav_u8);
		break;
	case DW_FORM_data2:
	case DW_FORM_ref2:
		error = dw_read_u16(dwbuf, &dav->dav_u16);
		break;
	case DW_FORM_data4:
	case DW_FORM_ref4:
		error = dw_read_u32(dwbuf, &dav->dav_u32);
		break;
	case DW_FORM_data8:
	case DW_FORM_ref8:
		error = dw_read_u64(dwbuf, &dav->dav_u64);
		break;
	case DW_FORM_ref_udata:
	case DW_FORM_udata:
		error = dw_read_uleb128(dwbuf, &dav->dav_u64);
		break;
	case DW_FORM_sdata:
		error = dw_read_sleb128(dwbuf, &dav->dav_s64);
		break;
	case DW_FORM_string:
		error = dw_read_string(dwbuf, &dav->dav_str);
		break;
	case DW_FORM_strp:
		error = dw_read_u32(dwbuf, &dav->dav_u32);
		break;
	case DW_FORM_flag_present:
		dav->dav_u8 = 1;
		break;
	default:
		error = ENOENT;
		break;
	}

	if (error) {
		pfree(&dav_pool, dav);
		return error;
	}

	STAILQ_INSERT_TAIL(davq, dav, dav_next);
	return 0;
}

static void
dw_attr_purge(struct dwaval_queue *davq)
{
	struct dwaval	*dav;

	while ((dav = STAILQ_FIRST(davq)) != NULL) {
		STAILQ_REMOVE_HEAD(davq, dav_next);
		pfree(&dav_pool, dav);
	}

	STAILQ_INIT(davq);
}

static int
dw_die_parse(struct dwbuf *dwbuf, size_t nextoff, uint8_t psz,
    struct dwabbrev_queue *dabq, struct dwdie_queue *dieq)
{
	struct dwdie	*die;
	struct dwabbrev	*dab;
	struct dwattr	*dat;
	uint64_t	 code;
	size_t		 doff;
	uint8_t		 lvl = 0;
	int		 error;


	while (dwbuf->len > 0) {
		doff = nextoff - dwbuf->len;
		if (dw_read_uleb128(dwbuf, &code))
			return -1;

		if (code == 0) {
			lvl--;
			continue;
		}

		STAILQ_FOREACH(dab, dabq, dab_next) {
			if (dab->dab_code == code)
				break;
		}
		if (dab == NULL)
			return ESRCH;

		die = pmalloc(&die_pool, sizeof(*die));
		if (die == NULL)
			return ENOMEM;

		die->die_lvl = lvl;
		die->die_dab = dab;
		die->die_offset = doff;
		STAILQ_INIT(&die->die_avals);

		STAILQ_FOREACH(dat, &dab->dab_attrs, dat_next) {
			error = dw_attr_parse(dwbuf, dat, psz, &die->die_avals);
			if (error != 0) {
				dw_attr_purge(&die->die_avals);
				return error;
			}
		}

		if (dab->dab_children == DW_CHILDREN_yes)
			lvl++;

		STAILQ_INSERT_TAIL(dieq, die, die_next);
	}

	return 0;
}

static void
dw_die_purge(struct dwdie_queue *dieq)
{
	struct dwdie	*die;

	while ((die = STAILQ_FIRST(dieq)) != NULL) {
		STAILQ_REMOVE_HEAD(dieq, die_next);
		dw_attr_purge(&die->die_avals);
		pfree(&die_pool, die);
	}

	STAILQ_INIT(dieq);
}

int
dw_ab_parse(struct dwbuf *abseg, struct dwabbrev_queue *dabq)
{
	struct dwabbrev	*dab;
	uint64_t	 code, tag;
	uint8_t		 children;

	if (abseg->len == 0)
		return EINVAL;

	for (;;) {
		if (dw_read_uleb128(abseg, &code) || (code == 0))
			break;

		if (dw_read_uleb128(abseg, &tag) ||
		    dw_read_u8(abseg, &children))
			return -1;

		dab = pmalloc(&dab_pool, sizeof(*dab));
		if (dab == NULL)
			return ENOMEM;

		dab->dab_code = code;
		dab->dab_tag = tag;
		dab->dab_children = children;
		STAILQ_INIT(&dab->dab_attrs);

		STAILQ_INSERT_TAIL(dabq, dab, dab_next);

		for (;;) {
			struct dwattr *dat;
			uint64_t attr = 0, form = 0;

			if (dw_read_uleb128(abseg, &attr) ||
			    dw_read_uleb128(abseg, &form))
				return -1;

			if ((attr == 0) && (form == 0))
				break;

			dat = pmalloc(&dat_pool, sizeof(*dat));
			if (dat == NULL)
				return ENOMEM;

			dat->dat_attr = attr;
			dat->dat_form = form;

			STAILQ_INSERT_TAIL(&dab->dab_attrs, dat, dat_next);
		}
	}

	return 0;
}

void
dw_dabq_purge(struct dwabbrev_queue *dabq)
{
	struct dwabbrev	*dab;

	while ((dab = STAILQ_FIRST(dabq)) != NULL) {
		struct dwattr *dat;

		STAILQ_REMOVE_HEAD(dabq, dab_next);
		while ((dat = STAILQ_FIRST(&dab->dab_attrs)) != NULL) {
			STAILQ_REMOVE_HEAD(&dab->dab_attrs, dat_next);
			pfree(&dat_pool, dat);
		}

		pfree(&dab_pool, dab);
	}

	STAILQ_INIT(dabq);
}

int
dw_cu_parse(struct dwbuf *info, struct dwbuf *abbrev, size_t seglen,
    struct dwcu **dcup)
{
	struct dwbuf	 abseg = *abbrev;
	struct dwbuf	 dwbuf;
	size_t		 segoff, nextoff;
	struct dwcu	*dcu = NULL;
	uint32_t	 length = 0, abbroff = 0;
	uint16_t	 version;
	uint8_t		 psz;
	int		 error;
#ifndef NOPOOL
	static int 	 dw_pool_inited = 0;

	if (!dw_pool_inited) {
		pool_init(&dcu_pool, "dcu", 1, sizeof(struct dwcu));
		pool_init(&dab_pool, "dab", 32, sizeof(struct dwabbrev));
		pool_init(&dat_pool, "dat", 32, sizeof(struct dwattr));
		pool_init(&die_pool, "die", 512, sizeof(struct dwdie));
		pool_init(&dav_pool, "dav", 1024, sizeof(struct dwaval));
		dw_pool_inited = 1;
	}
#endif /* NOPOOL */

	if (info->len == 0 || abbrev->len == 0)
		return EINVAL;

	/* Offset in the segment of the current Compile Unit. */
	segoff = seglen - info->len;

	if (dw_read_u32(info, &length))
		return -1;

	if (length >= 0xfffffff0 || length > info->len)
		return EOVERFLOW;

	/* Offset of the next Compile Unit. */
	nextoff = segoff + length + sizeof(uint32_t);

	if (dw_read_buf(info, &dwbuf, length))
		return -1;

	/* 32-bit DWARF only: the abbreviation offset is 4 bytes. */
	if (dw_read_u16(&dwbuf, &version) ||
	    dw_read_u32(&dwbuf, &abbroff) ||
	    dw_read_u8(&dwbuf, &psz))
		return -1;

	if (dw_skip_bytes(&abseg, abbroff))
		return -1;

	/* Only DWARF2 until extended. */
	if (version != 2)
		return ENOTSUP;

	dcu = pmalloc(&dcu_pool, sizeof(*dcu));
	if (dcu == NULL)
		return ENOMEM;

	dcu->dcu_offset = segoff;
	dcu->dcu_length = length;
	dcu->dcu_version = version;
	dcu->dcu_abbroff = abbroff;
	dcu->dcu_psize = psz;
	STAILQ_INIT(&dcu->dcu_abbrevs);
	STAILQ_INIT(&dcu->dcu_dies);

	error = dw_ab_parse(&abseg, &dcu->dcu_abbrevs);
	if (error != 0) {
		dw_dcu_free(dcu);
		return error;
	}

	error = dw_die_parse(&dwbuf, nextoff, psz, &dcu->dcu_abbrevs,
	    &dcu->dcu_dies);
	if (error != 0) {
		dw_dcu_free(dcu);
		return error;
	}

	if (dcup != NULL)
		*dcup = dcu;
	else
		dw_dcu_free(dcu);

	return 0;
}

void
dw_dcu_free(struct dwcu *dcu)
{
	if (dcu == NULL)
		return;

	dw_die_purge(&dcu->dcu_dies);
	dw_dabq_purge(&dcu->dcu_abbrevs);
	pfree(&dcu_pool, dcu);
}

int
dw_loc_parse(struct dwbuf *dwbuf, uint8_t *pop, uint64_t *poper1,
    uint64_t *poper2)
{
	uint64_t oper1 = 0, oper2 = 0;
	int64_t soper;
	uint8_t op;

	if (dw_read_u8(dwbuf, &op))
		return -1;

	if (pop != NULL)
		*pop = op;

	switch (op) {
	case DW_OP_constu:
	case DW_OP_plus_uconst:
	case DW_OP_regx:
	case DW_OP_piece:
		dw_read_uleb128(dwbuf, &oper1);
		break;

	case DW_OP_consts:
	case DW_OP_breg0 ... DW_OP_breg31:
	case DW_OP_fbreg:
		dw_read_sleb128(dwbuf, &soper);
		oper1 = (uint64_t)soper;
		break;
	default:
		return ENOTSUP;
	}

	if (poper1 != NULL)
		*poper1 = oper1;
	if (poper2 != NULL)
		*poper2 = oper2;

	return 0;
}
