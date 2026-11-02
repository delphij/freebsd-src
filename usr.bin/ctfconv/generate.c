/*	$OpenBSD: generate.c,v 1.8 2024/10/02 12:31:33 claudio Exp $ */

/*
 * Copyright (c) 2017 Martin Pieuchot
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

#include <sys/ctf.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <zlib.h>

#include "itype.h"
#include "xmalloc.h"
#include "hash.h"
#include "word.h"

#define ROUNDUP(x, y) ((((x) + (y) - 1) / (y)) * (y))

/*
 * The container (CTF version 3; version 2 only while it costs nothing):
 *
 * - Header, 36 bytes: magic 0xcff1, version, flags (CTF_F_COMPRESS),
 *   parent label and name (always 0: a non-zero parent name makes
 *   libctf look for a parent container, which FreeBSD never uses),
 *   then the offsets of the label, object, function, type and string
 *   sections relative to the end of the header, and the string table
 *   length.  The readers require the offsets to be non-decreasing and
 *   the object, function and type sections 4-byte aligned.
 * - Label section: one 8-byte entry when -l is given, naming the
 *   highest type ID; the build always passes one.
 * - Object and function sections: see the positional contract above
 *   elf_sort() in ctfconv.c.  A function record is info (kind FUNCTION,
 *   argument count), return type, argument types; ``...'' is not
 *   recorded in a record, only in a function type.
 * - Type section: IDs 1..N in emission order with no holes.  A record
 *   is name, info (kind, isroot, vlen) and either a size (integer,
 *   float, array, struct, union, enum) or a type ID (pointer, typedef,
 *   qualifiers, function's return; a forward's kind).  A size that does
 *   not fit takes the long form.  The variable part follows, by kind:
 *   an encoding word for integers and floats, contents/index/count for
 *   arrays, argument IDs for functions (padded to 4), members (short or
 *   long form by the aggregate's size) for structs and unions, name and
 *   int32 value for enumerators.  Readers walk the section with exactly
 *   these sizes; disagreeing misreads every later type.
 * - String table: byte 0 is NUL; every name is an offset into it with
 *   bit 31 clear (set would mean the ELF string table, never used).
 * Everything after the header is one zlib stream; the readers compute
 * the inflated size as stroff + strlen and reject a stream of another
 * length.  The container is written in the target's byte order.
 *
 * What is emitted (types_finalize() in parse.c decides): every type
 * reached from any converted C unit, not only those reachable from
 * symbols, so that D scripts can use sizeof (struct foo) for types no
 * symbol has; types reachable only from dropped duplicates are not.
 * IDs follow first occurrence, so the same input gives a byte-identical
 * container on every run and host.
 */

/*
 * Dynamic buffer, used for content & string table.
 */
struct dbuf {
	char		*data;	/* start data buffer */
	size_t		 size;	/* size of the buffer */

	char		*cptr; /* position in [data, data + size] */
	size_t		 coff; /* number of written bytes */
};

#define DBUF_CHUNKSZ	(64 * 1024)

/* In-memory representation of a CTF section. */
struct imcs {
	struct dbuf	 body;
	struct dbuf	 stab;	/* corresponding string table */
	struct hash	*htab;	/* hash table of known strings */
};

struct strentry {
	struct hash_entry se_key;	/* Must be first */
#define se_str se_key.hkey
	size_t		 se_off;
};

static char	*data_compress(const char *, size_t, size_t *);

static void
dbuf_realloc(struct dbuf *dbuf, size_t len)
{
	assert(dbuf != NULL);
	assert(len != 0);

	dbuf->data = xrealloc(dbuf->data, dbuf->size + len);
	dbuf->size += len;
	dbuf->cptr = dbuf->data + dbuf->coff;
}

static void
dbuf_copy(struct dbuf *dbuf, void const *data, size_t len)
{
	size_t left;

	assert(dbuf->cptr != NULL);
	assert(dbuf->data != NULL);
	assert(dbuf->size != 0);

	if (len == 0)
		return;

	left = dbuf->size - dbuf->coff;
	if (left < len)
		dbuf_realloc(dbuf, ROUNDUP((len - left), DBUF_CHUNKSZ));

	memcpy(dbuf->cptr, data, len);
	dbuf->cptr += len;
	dbuf->coff += len;
}

static size_t
dbuf_pad(struct dbuf *dbuf, int align)
{
	int i = (align - (dbuf->coff % align)) % align;

	while (i-- > 0)
		dbuf_copy(dbuf, "", 1);

	return dbuf->coff;
}

/*
 * Everything after the header is written in the byte order of the
 * target, one field at a time, so that the output never depends on
 * the host.
 */
static void
dbuf_put8(struct dbuf *dbuf, uint8_t v)
{
	dbuf_copy(dbuf, &v, sizeof(v));
}

static void
dbuf_put16(struct dbuf *dbuf, uint16_t v)
{
	if (target_bigendian != host_bigendian())
		v = (uint16_t)((v >> 8) | (v << 8));
	dbuf_copy(dbuf, &v, sizeof(v));
}

static void
dbuf_put32(struct dbuf *dbuf, uint32_t v)
{
	if (target_bigendian != host_bigendian())
		v = ((v >> 24) & 0xff) | ((v >> 8) & 0xff00) |
		    ((v << 8) & 0xff0000) | (v << 24);
	dbuf_copy(dbuf, &v, sizeof(v));
}

/* One type ID: 16 bits in version 2, 32 bits in version 3. */
static void
dbuf_putid(struct dbuf *dbuf, uint32_t id)
{
	if (ctf_version == CTF_VERSION_2) {
		assert(id <= UINT16_MAX);
		dbuf_put16(dbuf, (uint16_t)id);
	} else
		dbuf_put32(dbuf, id);
}

static uint32_t
type_info(int kind, int root, uint32_t vlen)
{
	if (ctf_version == CTF_VERSION_2)
		return CTF_V2_TYPE_INFO(kind, root, vlen);
	return CTF_V3_TYPE_INFO(kind, root, vlen);
}

static uint32_t
max_vlen(void)
{
	return (ctf_version == CTF_VERSION_2) ? CTF_MAX_VLEN : CTF_V3_MAX_VLEN;
}

static uint32_t
max_size(void)
{
	return (ctf_version == CTF_VERSION_2) ? CTF_MAX_SIZE : CTF_V3_MAX_SIZE;
}

static uint32_t
lsize_sent(void)
{
	return (ctf_version == CTF_VERSION_2) ? CTF_LSIZE_SENT :
	    CTF_V3_LSIZE_SENT;
}

static uint64_t
lstruct_thresh(void)
{
	return (ctf_version == CTF_VERSION_2) ? CTF_LSTRUCT_THRESH :
	    CTF_V3_LSTRUCT_THRESH;
}

/* The readers require the v3 sections on a 4-byte boundary. */
static int
section_align(void)
{
	return (ctf_version == CTF_VERSION_2) ? 2 : 4;
}

static const char *
type_label(struct itype *it)
{
	const char *name = it_name(it);

	return (name != NULL) ? name : "(anon)";
}

/*
 * Size in bytes of a type, following references.  Arrays need this
 * because DWARF does not record their size.
 */
static uint64_t
it_bytesize(struct itype *it)
{
	int depth;

	for (depth = 0; it != NULL && depth < 64; depth++) {
		if (it->it_flags & ITF_FORWARD)
			return 0;

		switch (it->it_type) {
		case CTF_K_INTEGER:
		case CTF_K_FLOAT:
		case CTF_K_STRUCT:
		case CTF_K_UNION:
		case CTF_K_ENUM:
			return it->it_size;
		case CTF_K_POINTER:
			return target_psz;
		case CTF_K_ARRAY:
			return (uint64_t)it->it_nelems * it_bytesize(it->it_refp);
		case CTF_K_TYPEDEF:
		case CTF_K_VOLATILE:
		case CTF_K_CONST:
		case CTF_K_RESTRICT:
			it = it->it_refp;
			break;
		default:
			return 0;
		}
	}

	return 0;
}

static size_t
imcs_add_string(struct imcs *imcs, const char *str)
{
	struct strentry *se;
	unsigned int slot;

	if (str == NULL || *str == '\0')
		return 0;

	se = (struct strentry *)hash_find(imcs->htab, str, &slot);
	if (se == NULL) {
		se = xmalloc(sizeof(*se));
		hash_insert(imcs->htab, slot, &se->se_key, str);
		se->se_off = imcs->stab.coff;

		dbuf_copy(&imcs->stab, str, strlen(str) + 1);
	}

	if (se->se_off > CTF_MAX_NAME)
		errx(1, "string table too large for \"%s\"", str);

	return se->se_off;
}

/*
 * Function record: info, return type, argument types.
 * A symbol without type information is a single zero word.  ``...''
 * is not recorded here, only in function types.
 */
static void
imcs_add_func(struct imcs *imcs, struct itype *it)
{
	struct imember		*im;
	uint32_t		 vlen;

	if (it->it_type == CTF_K_UNKNOWN) {
		dbuf_putid(&imcs->body, 0);
		return;
	}
	if (it->it_refp == NULL)
		errx(1, "internal error: function %s has no return type",
		    type_label(it));

	vlen = it->it_nelems;
	if (vlen > max_vlen())
		errx(1, "function %s: too many arguments (%u)", type_label(it),
		    vlen);

	dbuf_putid(&imcs->body, type_info(CTF_K_FUNCTION, 0, vlen));
	dbuf_putid(&imcs->body, it->it_refp->it_idx);

	TAILQ_FOREACH(im, &it->it_members, im_next)
		dbuf_putid(&imcs->body, im->im_refp->it_idx);
}

static void
imcs_add_obj(struct imcs *imcs, struct itype *it)
{
	if (it->it_refp == NULL)
		errx(1, "internal error: object %s has no type",
		    type_label(it));
	dbuf_putid(&imcs->body, it->it_refp->it_idx);
}

/*
 * Member list of a struct or union.
 */
static void
imcs_add_members(struct imcs *imcs, struct itype *it, uint64_t size)
{
	struct imember		*im;
	uint32_t		 name;

	TAILQ_FOREACH(im, &it->it_members, im_next) {
		name = imcs_add_string(imcs, im_name(im));

		if (size < lstruct_thresh()) {
			dbuf_put32(&imcs->body, name);
			if (ctf_version == CTF_VERSION_2) {
				dbuf_put16(&imcs->body, im->im_refp->it_idx);
				dbuf_put16(&imcs->body, im->im_off);
			} else {
				dbuf_put32(&imcs->body, im->im_refp->it_idx);
				dbuf_put32(&imcs->body, im->im_off);
			}
		} else {
			dbuf_put32(&imcs->body, name);
			if (ctf_version == CTF_VERSION_2) {
				dbuf_put16(&imcs->body, im->im_refp->it_idx);
				dbuf_put16(&imcs->body, 0);
			} else
				dbuf_put32(&imcs->body, im->im_refp->it_idx);
			dbuf_put32(&imcs->body, CTF_OFFSET_TO_LMEMHI(im->im_off));
			dbuf_put32(&imcs->body, CTF_OFFSET_TO_LMEMLO(im->im_off));
		}
	}
}

static void
imcs_add_type(struct imcs *imcs, struct itype *it)
{
	struct imember		*im;
	uint64_t		 size;
	uint32_t		 vlen, name;
	int			 kind, root;

	assert(it->it_type != CTF_K_UNKNOWN);

	kind = it->it_type;
	vlen = it->it_nelems;
	size = it->it_size;
	root = (it->it_flags & ITF_ROOT) ? 1 : 0;

	if (it->it_flags & ITF_FORWARD) {
		kind = CTF_K_FORWARD;
		vlen = 0;
		stats.st_fwd_left++;
	} else if (kind == CTF_K_FUNCTION && (it->it_flags & ITF_VARARGS))
		vlen++;
	else if (kind == CTF_K_ARRAY) {
		vlen = 1;
		size = it_bytesize(it);
	} else if (kind == CTF_K_INTEGER || kind == CTF_K_FLOAT)
		vlen = 1;

	if (vlen > max_vlen())
		errx(1, "type %s: too many members (%u)", type_label(it), vlen);

	name = imcs_add_string(imcs, it_name(it));
	dbuf_put32(&imcs->body, name);
	dbuf_putid(&imcs->body, type_info(kind, root, vlen));

	switch (kind) {
	case CTF_K_FORWARD:
		/* The third word carries the kind of the tag. */
		dbuf_putid(&imcs->body, it->it_type);
		break;
	case CTF_K_POINTER:
	case CTF_K_TYPEDEF:
	case CTF_K_VOLATILE:
	case CTF_K_CONST:
	case CTF_K_RESTRICT:
	case CTF_K_FUNCTION:
		assert(it->it_refp != NULL);
		dbuf_putid(&imcs->body, it->it_refp->it_idx);
		break;
	default:
		/* Sized kinds. */
		if (size <= max_size())
			dbuf_putid(&imcs->body, size);
		else {
			dbuf_putid(&imcs->body, lsize_sent());
			dbuf_put32(&imcs->body, CTF_SIZE_TO_LSIZE_HI(size));
			dbuf_put32(&imcs->body, CTF_SIZE_TO_LSIZE_LO(size));
		}
		break;
	}

	switch (kind) {
	case CTF_K_INTEGER:
	case CTF_K_FLOAT:
		assert(it->it_bits <= UINT16_MAX);
		dbuf_put32(&imcs->body, CTF_INT_DATA(it->it_enc, 0, it->it_bits));
		break;
	case CTF_K_ARRAY:
		assert(long_it != NULL);
		dbuf_putid(&imcs->body, it->it_refp->it_idx);
		dbuf_putid(&imcs->body, long_it->it_idx);
		dbuf_put32(&imcs->body, it->it_nelems);
		break;
	case CTF_K_STRUCT:
	case CTF_K_UNION:
		imcs_add_members(imcs, it, size);
		break;
	case CTF_K_FUNCTION:
		TAILQ_FOREACH(im, &it->it_members, im_next)
			dbuf_putid(&imcs->body, im->im_refp->it_idx);
		if (it->it_flags & ITF_VARARGS)
			dbuf_putid(&imcs->body, 0);
		/* v3 IDs are 4 bytes, so padding only matters for v2. */
		dbuf_pad(&imcs->body, 4);
		break;
	case CTF_K_ENUM:
		TAILQ_FOREACH(im, &it->it_members, im_next) {
			dbuf_put32(&imcs->body,
			    imcs_add_string(imcs, im_name(im)));
			dbuf_put32(&imcs->body, (uint32_t)(int32_t)im->im_ref);
		}
		break;
	default:
		break;
	}

	stats.st_types_out++;
}

/*
 * isroot has no effect on any FreeBSD reader (only ctfdump shows it); it
 * is set for every named integer, float, struct, union and enum, every
 * typedef, the type of every typed object and the return type of every
 * typed function, and clear otherwise.
 */
static void
imcs_mark_roots(void)
{
	struct itype		*it;

	TAILQ_FOREACH(it, &itypeq, it_next) {
		if (it->it_flags & (ITF_FUNC|ITF_OBJ))
			continue;

		switch (it->it_type) {
		case CTF_K_INTEGER:
		case CTF_K_FLOAT:
		case CTF_K_STRUCT:
		case CTF_K_UNION:
		case CTF_K_ENUM:
			if (it_name(it) != NULL && !(it->it_flags & ITF_FORWARD))
				it->it_flags |= ITF_ROOT;
			break;
		case CTF_K_TYPEDEF:
			it->it_flags |= ITF_ROOT;
			break;
		default:
			break;
		}
	}

	TAILQ_FOREACH(it, &iobjq, it_symb) {
		if (it->it_refp != NULL && it->it_refp != it)
			it->it_refp->it_flags |= ITF_ROOT;
	}
	TAILQ_FOREACH(it, &ifuncq, it_symb) {
		if (it->it_type == CTF_K_FUNCTION && it->it_refp != NULL)
			it->it_refp->it_flags |= ITF_ROOT;
	}
}

static void
imcs_generate(struct imcs *imcs, struct ctf_header *cth, const char *label)
{
	struct itype		*it;

	memset(imcs, 0, sizeof(*imcs));

	dbuf_realloc(&imcs->body, DBUF_CHUNKSZ);
	dbuf_realloc(&imcs->stab, DBUF_CHUNKSZ);

	imcs->htab = hash_init(10);
	if (imcs->htab == NULL)
		err(1, "hash_init");

	/* Add empty string */
	dbuf_copy(&imcs->stab, "", 1);

	/* We don't use parent label */
	cth->cth_parlabel = 0;
	cth->cth_parname = 0;

	/* IDs with the top bit set belong to a child container. */
	if (CTF_V3_TYPE_ISCHILD(tidx) ||
	    (ctf_version == CTF_VERSION_2 && tidx > 0x7fff))
		errx(1, "too many types (%u) for CTF version %d", tidx,
		    ctf_version);

	imcs_mark_roots();

	/* Insert a single label for all types, if one was given. */
	cth->cth_lbloff = 0;
	if (label != NULL) {
		dbuf_put32(&imcs->body, imcs_add_string(imcs, label));
		dbuf_put32(&imcs->body, tidx);
	}

	/* Insert objects */
	cth->cth_objtoff = dbuf_pad(&imcs->body, section_align());
	TAILQ_FOREACH(it, &iobjq, it_symb)
		imcs_add_obj(imcs, it);

	/* Insert functions */
	cth->cth_funcoff = dbuf_pad(&imcs->body, section_align());
	TAILQ_FOREACH(it, &ifuncq, it_symb)
		imcs_add_func(imcs, it);

	/* Insert types */
	cth->cth_typeoff = dbuf_pad(&imcs->body, 4);
	TAILQ_FOREACH(it, &itypeq, it_next) {
		if (it->it_flags & (ITF_FUNC|ITF_OBJ))
			continue;

		imcs_add_type(imcs, it);
	}

	/* String table is written from its own buffer. */
	cth->cth_stroff = imcs->body.coff;
	cth->cth_strlen = imcs->stab.coff;
}

/*
 * Serialize the header in target byte order.
 */
static void
header_put(struct dbuf *dbuf, const struct ctf_header *cth)
{
	dbuf_put16(dbuf, cth->cth_magic);
	dbuf_put8(dbuf, cth->cth_version);
	dbuf_put8(dbuf, cth->cth_flags);
	dbuf_put32(dbuf, cth->cth_parlabel);
	dbuf_put32(dbuf, cth->cth_parname);
	dbuf_put32(dbuf, cth->cth_lbloff);
	dbuf_put32(dbuf, cth->cth_objtoff);
	dbuf_put32(dbuf, cth->cth_funcoff);
	dbuf_put32(dbuf, cth->cth_typeoff);
	dbuf_put32(dbuf, cth->cth_stroff);
	dbuf_put32(dbuf, cth->cth_strlen);
}

/*
 * Generate a CTF buffer from the internal type representation.
 * Returns the malloc()ed container and its length, or -1.
 */
int
generate_data(const char *label, int compress, char **pdata, size_t *plen)
{
	char			*p, *ctfdata = NULL;
	ssize_t			 ctflen;
	struct ctf_header	 cth;
	struct imcs		 imcs;
	struct dbuf		 hdr;

	memset(&cth, 0, sizeof(cth));

	cth.cth_magic = CTF_MAGIC;
	cth.cth_version = ctf_version;

	if (compress)
		cth.cth_flags = CTF_F_COMPRESS;

	imcs_generate(&imcs, &cth, label);

	memset(&hdr, 0, sizeof(hdr));
	dbuf_realloc(&hdr, sizeof(cth));
	header_put(&hdr, &cth);
	assert(hdr.coff == sizeof(cth));

	ctflen = sizeof(cth) + imcs.body.coff + imcs.stab.coff;
	p = ctfdata = xmalloc(ctflen);

	memcpy(p, hdr.data, sizeof(cth));
	p += sizeof(cth);
	free(hdr.data);

	memcpy(p, imcs.body.data, imcs.body.coff);
	p += imcs.body.coff;

	memcpy(p, imcs.stab.data, imcs.stab.coff);
	p += imcs.stab.coff;

	assert((p - ctfdata) == ctflen);

	if (compress) {
		char *cdata;
		size_t clen;

		cdata = data_compress(ctfdata + sizeof(cth),
		    ctflen - sizeof(cth), &clen);
		if (cdata == NULL) {
			warnx("compressing CTF data");
			free(ctfdata);
			return -1;
		}

		/*
		 * The deflate stream of a tiny body is larger than the
		 * body, so the container is rebuilt rather than patched.
		 */
		ctflen = clen + sizeof(cth);
		p = xmalloc(ctflen);
		memcpy(p, ctfdata, sizeof(cth));
		memcpy(p + sizeof(cth), cdata, clen);
		free(ctfdata);
		free(cdata);
		ctfdata = p;
	}

	free(imcs.body.data);
	free(imcs.stab.data);

	*pdata = ctfdata;
	*plen = ctflen;
	return 0;
}

/*
 * Raw mode: the container alone, in ``path'', which is ``base'' in the
 * directory open on ``dfd'' (the tool is in capability mode, see
 * sandbox() in ctfconv.c).
 */
int
generate(int dfd, const char *base, const char *path, const char *label,
    int compress)
{
	char			*ctfdata;
	size_t			 ctflen;
	int			 error = 0, fd;

	if (generate_data(label, compress, &ctfdata, &ctflen) == -1)
		return -1;

	fd = openat(dfd, base, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd == -1) {
		warn("open %s", path);
		free(ctfdata);
		return -1;
	}

	if (write(fd, ctfdata, ctflen) != (ssize_t)ctflen) {
		warn("unable to write %zu bytes for %s", ctflen, path);
		error = -1;
	}

	close(fd);
	free(ctfdata);
	return error;
}

/*
 * Deflate ``size'' bytes of ``buf'' in one call.  The output buffer is
 * sized by deflateBound(), since a stream can be longer than its
 * input; Z_OK from deflate(Z_FINISH) would mean it was still too small
 * and is not a zlib error code with a message.
 */
static char *
data_compress(const char *buf, size_t size, size_t *pclen)
{
	z_stream		 stream;
	char			*data = NULL;
	size_t			 len;
	int			 error;

	memset(&stream, 0, sizeof(stream));
	stream.zalloc = Z_NULL;
	stream.zfree = Z_NULL;
	stream.opaque = Z_NULL;

	if ((error = deflateInit(&stream, Z_BEST_COMPRESSION)) != Z_OK) {
		warnx("zlib deflateInit failed: %s", zError(error));
		return NULL;
	}

	len = deflateBound(&stream, size);
	data = malloc(len);
	if (data == NULL) {
		warn(NULL);
		deflateEnd(&stream);
		return NULL;
	}

	stream.next_in = __DECONST(unsigned char *, buf);
	stream.avail_in = size;
	stream.next_out = (unsigned char *)data;
	stream.avail_out = len;

	if ((error = deflate(&stream, Z_FINISH)) != Z_STREAM_END) {
		warnx("zlib deflate failed: %s", error == Z_OK ?
		    "output buffer too small" : zError(error));
		deflateEnd(&stream);
		goto exit;
	}

	if ((error = deflateEnd(&stream)) != Z_OK) {
		warnx("zlib deflateEnd failed: %s", zError(error));
		goto exit;
	}

	if (pclen != NULL)
		*pclen = stream.total_out;

	return data;

exit:
	free(data);
	return NULL;
}
