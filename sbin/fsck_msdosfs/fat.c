/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Google LLC
 * Copyright (C) 1995, 1996, 1997 Wolfgang Solfrank
 * Copyright (c) 1995 Martin Husemann
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/cdefs.h>
#ifndef lint
__RCSID("$NetBSD: fat.c,v 1.18 2006/06/05 16:51:18 christos Exp $");
static const char rcsid[] =
  "$FreeBSD$";
#endif /* not lint */

#include <sys/endian.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/param.h>

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>

#include "ext.h"
#include "fsutil.h"

static int _readfat(int, struct bootblock *, u_char **);

static bool is_mmapped;

/*
 * Used and head bitmaps for FAT scanning.
 *
 * FAT32 have up to 2^28 = 256M entries, and FAT16/12 have much less.
 * For each cluster, we use 1 bit to represent if it's "used"
 * (referenced by any file or directory), and another to represent if
 * it's a head cluster (the first cluster of a cluster chain).
 *
 * Head bitmap
 * ===========
 * Initially, we set all bits to 1.  In readfat(), we traverse the
 * whole FAT and mark each cluster identified as "next" cluster as
 * 0.  After the scan, we have a bitmap with 1's to indicate the
 * corresponding cluster was a "head" cluster.
 *
 * We use head bitmap to identify lost chains: a head cluster that was
 * not being claimed by any file or directories is the head cluster of
 * a lost chain.
 *
 * Used bitmap
 * ===========
 * Initially, we set all bits to 0.  As we traverse the directory
 * structure, we first check if the head cluster referenced by the
 * directory entry was a head cluster, and if it was, we mark the
 * whole chain as being used and clear the head map bit.
 *
 * The used bitmap have two purposes: first, we can immediately find
 * out a cross chain because the node must have been already marked
 * as used in a previous scan; second, if we do not care about lost
 * chains, the data can immediately be used for clearing the unclaimed
 * yet non-zero clusters from the FAT, similar to a "mark and sweep"
 * garbage collection.
 *
 * Handle of lost chains
 * =====================
 * At the end of scanning, we can easily find all lost chain's heads
 * by finding out the 1's in the head bitmap.
 */
typedef struct long_bitmap {
	size_t		 count;
	unsigned long	*map;
} long_bitmap_t;
static long_bitmap_t	usedbitmap;
static long_bitmap_t	headbitmap;

static inline void
bitmap_set(long_bitmap_t *lbp, cl_t cl)
{
	cl_t i = cl / LONG_BIT;
	unsigned long setbit = 1UL << (cl % LONG_BIT);

	assert((lbp->map[i] & setbit) == 0);
	lbp->map[i] |= setbit;
	lbp->count++;

}

static inline void
bitmap_clear(long_bitmap_t *lbp, cl_t cl)
{
	cl_t i = cl / LONG_BIT;
	unsigned long clearmask = ~(1UL << (cl % LONG_BIT));

	assert((lbp->map[i] & ~clearmask) != 0);
	lbp->map[i] &= clearmask;
	lbp->count--;
}

static inline bool
bitmap_get(long_bitmap_t *lbp, cl_t cl)
{
	cl_t i = cl / LONG_BIT;
	unsigned long usedbit = 1UL << (cl % LONG_BIT);

	return ((lbp->map[i] & usedbit) == usedbit);
}

static inline bool
bitmap_any_in_range(long_bitmap_t *lbp, cl_t cl)
{
	cl_t i = cl / LONG_BIT;

	return (lbp->map[i] == 0);
}

static inline size_t
bitmap_count(long_bitmap_t *lbp)
{
	return (lbp->count);
}

static int
bitmap_ctor(long_bitmap_t *lbp, size_t bits, bool allone)
{
	size_t bitmap_size = roundup2(bits, LONG_BIT) / (LONG_BIT / 8);

	free(lbp->map);
	lbp->map = calloc(1, bitmap_size);
	if (lbp->map == NULL)
		return FSFATAL;

	if (allone) {
		memset(lbp->map, 0xff, bitmap_size);
		lbp->count = bits;
	} else {
		lbp->count = 0;
	}
	return FSOK;
}

static void
bitmap_dtor(long_bitmap_t *lbp)
{
	free(lbp->map);
	lbp->map = NULL;
}

void
fat_set_cl_used(cl_t cl)
{
	bitmap_set(&usedbitmap, cl);
}

void
fat_clear_cl_used(cl_t cl)
{
	bitmap_clear(&usedbitmap, cl);
}

bool
fat_is_cl_used(cl_t cl)
{
	return (bitmap_get(&usedbitmap, cl));
}

void
fat_clear_cl_head(cl_t cl)
{
	bitmap_clear(&headbitmap, cl);
}

bool
fat_is_cl_head(cl_t cl)
{
	return (bitmap_get(&headbitmap, cl));
}

static inline bool
fat_is_cl_head_in_range(cl_t cl)
{
	return (bitmap_any_in_range(&headbitmap, cl));
}

static size_t
fat_get_head_count(void)
{
	return (bitmap_count(&headbitmap));
}

/*
 * FAT table descriptor, represents a FAT table that is already loaded
 * into memory.
 */
struct fat_descriptor {
	struct bootblock *boot;
	size_t		  fatsize;
	uint8_t		 *fatbuf;
};

/*
 * FAT12 accessors.
 *
 * FAT12s are sufficiently small, expect it to always fit in the RAM.
 */
static uint8_t *
fat_get_fat12_ptr(struct fat_descriptor *fat, cl_t cl)
{
	return (fat->fatbuf + ((cl + (cl >> 1))));
}

static cl_t
fat_get_fat12_next(struct fat_descriptor *fat, cl_t cl)
{
	const uint8_t	*p;
	cl_t	retval;

	p = fat_get_fat12_ptr(fat, cl);
	retval = le16dec(p);
	/* Odd cluster: lower 4 bits belongs to the subsequent cluster */
	if ((cl & 1) == 1)
		retval >>= 4;
	retval &= CLUST12_MASK;

	if (retval >= (CLUST_BAD & CLUST12_MASK))
		retval |= ~CLUST12_MASK;

	return (retval);
}

static int
fat_set_fat12_next(struct fat_descriptor *fat, cl_t cl, cl_t nextcl)
{
	uint8_t	*p;

	/* Truncate 'nextcl' value, if needed */
	nextcl &= CLUST12_MASK;

	p = fat_get_fat12_ptr(fat, cl);

	/*
	 * Read in the 4 bits from the subsequent (for even clusters)
	 * or the preceding (for odd clusters) cluster and combine
	 * it to the nextcl value for encoding
	 */
	if ((cl & 1) == 0) {
		nextcl |= ((p[1] & 0xf0) << 8);
	} else {
		nextcl <<= 4;
		nextcl |= (p[0] & 0x0f);
	}

	le16enc(p, (uint16_t)nextcl);

	return (0);
}

/*
 * FAT16 accessors.
 *
 * FAT16s are sufficiently small, expect it to always fit in the RAM.
 */
static uint8_t *
fat_get_fat16_ptr(struct fat_descriptor *fat, cl_t cl)
{
	return (fat->fatbuf + (cl << 1));
}

static cl_t
fat_get_fat16_next(struct fat_descriptor *fat, cl_t cl)
{
	const uint8_t	*p;
	cl_t	retval;

	p = fat_get_fat16_ptr(fat, cl);
	retval = le16dec(p) & CLUST16_MASK;

	if (retval >= (CLUST_BAD & CLUST16_MASK))
		retval |= ~CLUST16_MASK;

	return (retval);
}

static int
fat_set_fat16_next(struct fat_descriptor *fat, cl_t cl, cl_t nextcl)
{
	uint8_t	*p;

	/* Truncate 'nextcl' value, if needed */
	nextcl &= CLUST16_MASK;

	p = fat_get_fat16_ptr(fat, cl);

	le16enc(p, (uint16_t)nextcl);

	return (0);
}

/*
 * FAT32 accessors.
 *
 * TODO(delphij): paging support
 */
static uint8_t *
fat_get_fat32_ptr(struct fat_descriptor *fat, cl_t cl, bool write __unused)
{
	return (fat->fatbuf + (cl << 2));
}

static cl_t
fat_get_fat32_next(struct fat_descriptor *fat, cl_t cl)
{
	const uint8_t	*p;
	cl_t	retval;

	p = fat_get_fat32_ptr(fat, cl, false);
	retval = le32dec(p) & CLUST32_MASK;

	if (retval >= (CLUST_BAD & CLUST32_MASK))
		retval |= ~CLUST32_MASK;

	return (retval);
}

static int
fat_set_fat32_next(struct fat_descriptor *fat, cl_t cl, cl_t nextcl)
{
	uint8_t	*p;

	/* Truncate 'nextcl' value, if needed */
	nextcl &= CLUST32_MASK;

	p = fat_get_fat32_ptr(fat, cl, true);

	le32enc(p, (uint32_t)nextcl);

	return (0);
}

/*
 * Generic accessor interface for FAT
 */
cl_t fat_get_cl_next(struct fat_descriptor *fat, cl_t cl)
{
	cl_t retval = CLUST_DEAD;

	if (cl < CLUST_FIRST || cl >= fat->boot->NumClusters) {
		pfatal("Invalid cluster: %ud", cl);
		return CLUST_DEAD;
	}

	switch (fat->boot->ClustMask) {
	case CLUST12_MASK:
		retval = fat_get_fat12_next(fat, cl);
		break;
	case CLUST16_MASK:
		retval = fat_get_fat16_next(fat, cl);
		break;
	case CLUST32_MASK:
		retval = fat_get_fat32_next(fat, cl);
		break;
	default:
		pfatal("Invalid ClustMask: %d", fat->boot->ClustMask);
		break;
	}
	return (retval);
}

int fat_set_cl_next(struct fat_descriptor *fat, cl_t cl, cl_t nextcl)
{
	int retval = FSFATAL;

	if (rdonly) {
		pwarn(" (NO WRITE)\n");
		return FSFATAL;
	}

	if (cl < CLUST_FIRST || cl >= fat->boot->NumClusters) {
		pfatal("Invalid cluster: %ud", cl);
		return FSFATAL;
	}

	switch (fat->boot->ClustMask) {
	case CLUST12_MASK:
		retval = fat_set_fat12_next(fat, cl, nextcl);
		break;
	case CLUST16_MASK:
		retval = fat_set_fat16_next(fat, cl, nextcl);
		break;
	case CLUST32_MASK:
		retval = fat_set_fat32_next(fat, cl, nextcl);
		break;
	default:
		pfatal("Invalid ClustMask: %d", fat->boot->ClustMask);
		break;
	}
	return (retval);
}

struct bootblock*
fat_get_boot(struct fat_descriptor *fat) {

	return(fat->boot);
}

/*
 * The first 2 FAT entries contain pseudo-cluster numbers with the following
 * layout:
 *
 * 31...... ........ ........ .......0
 * rrrr1111 11111111 11111111 mmmmmmmm         FAT32 entry 0
 * rrrrsh11 11111111 11111111 11111xxx         FAT32 entry 1
 *
 *                   11111111 mmmmmmmm         FAT16 entry 0
 *                   sh111111 11111xxx         FAT16 entry 1
 *
 * r = reserved
 * m = BPB media ID byte
 * s = clean flag (1 = dismounted; 0 = still mounted)
 * h = hard error flag (1 = ok; 0 = I/O error)
 * x = any value ok
 */

int
checkdirty(int fs, struct bootblock *boot)
{
	off_t off;
	u_char *buffer;
	int ret = 0;
	size_t len;

	if (boot->ClustMask != CLUST16_MASK && boot->ClustMask != CLUST32_MASK)
		return 0;

	off = boot->bpbResSectors;
	off *= boot->bpbBytesPerSec;

	buffer = malloc(len = boot->bpbBytesPerSec);
	if (buffer == NULL) {
		perr("No space for FAT sectors (%zu)", len);
		return 1;
	}

	if (lseek(fs, off, SEEK_SET) != off) {
		perr("Unable to read FAT");
		goto err;
	}

	if ((size_t)read(fs, buffer, boot->bpbBytesPerSec) !=
	    boot->bpbBytesPerSec) {
		perr("Unable to read FAT");
		goto err;
	}

	/*
	 * If we don't understand the FAT, then the file system must be
	 * assumed to be unclean.
	 */
	if (buffer[0] != boot->bpbMedia || buffer[1] != 0xff)
		goto err;
	if (boot->ClustMask == CLUST16_MASK) {
		if ((buffer[2] & 0xf8) != 0xf8 || (buffer[3] & 0x3f) != 0x3f)
			goto err;
	} else {
		if (buffer[2] != 0xff || (buffer[3] & 0x0f) != 0x0f
		    || (buffer[4] & 0xf8) != 0xf8 || buffer[5] != 0xff
		    || buffer[6] != 0xff || (buffer[7] & 0x03) != 0x03)
			goto err;
	}

	/*
	 * Now check the actual clean flag (and the no-error flag).
	 */
	if (boot->ClustMask == CLUST16_MASK) {
		if ((buffer[3] & 0xc0) == 0xc0)
			ret = 1;
	} else {
		if ((buffer[7] & 0x0c) == 0x0c)
			ret = 1;
	}

err:
	free(buffer);
	return ret;
}

/*
 * Read a FAT from disk. Returns 1 if successful, 0 otherwise.
 */
static int
_readfat(int fs, struct bootblock *boot, u_char **buffer)
{
	off_t off;
	size_t fatsize;

	fatsize = boot->FATsecs * boot->bpbBytesPerSec;

	off = boot->bpbResSectors;
	off *= boot->bpbBytesPerSec;

	is_mmapped = false;

	/* Attempt to mmap() first */
	*buffer = mmap(NULL, fatsize, PROT_READ | (rdonly ? 0 : PROT_WRITE),
	    MAP_SHARED, fs, off);
	if (*buffer != MAP_FAILED) {
		is_mmapped = true;
		return 1;
	}

	/* mmap failed, create a buffer and read in the FAT table */
	*buffer = malloc(fatsize);
	if (*buffer == NULL) {
		perr("No space for FAT sectors (%zu)",
		    (size_t)boot->FATsecs);
		return 0;
	}

	if (lseek(fs, off, SEEK_SET) != off) {
		perr("Unable to read FAT");
		goto err;
	}

	if ((size_t)read(fs, *buffer,fatsize) != fatsize) {
		perr("Unable to read FAT");
		goto err;
	}

	return 1;

    err:
	free(*buffer);
	return 0;
}

static void
releasefat(struct bootblock *boot, u_char **buffer)
{
	size_t fatsize;

	if (is_mmapped) {
		fatsize = boot->FATsecs * boot->bpbBytesPerSec;
		munmap(*buffer, fatsize);
	} else {
		free(*buffer);
	}
	*buffer = NULL;
}

/*
 * Read or map a FAT and populate head bitmap
 */
int
readfat(int fs, struct bootblock *boot, struct fat_descriptor **fp)
{
	struct fat_descriptor *fat;
	u_char *buffer, *p;
	cl_t cl, nextcl;
	int ret = FSOK;

	boot->NumFree = boot->NumBad = 0;

	if (!_readfat(fs, boot, &buffer))
		return FSFATAL;

	fat = calloc(1, sizeof(struct fat_descriptor));
	if (fat == NULL) {
		perr("No space for FAT descriptor");
		releasefat(boot, &buffer);
		return FSFATAL;
	}

	if (bitmap_ctor(&usedbitmap, boot->NumClusters, false) != FSOK) {
		perr("No space for used bitmap for FAT clusters (%zu)",
		    (size_t)boot->NumClusters);
		releasefat(boot, &buffer);
		free(fat);
		return FSFATAL;
	}

	if (bitmap_ctor(&headbitmap, boot->NumClusters, true) != FSOK) {
		perr("No space for head bitmap for FAT clusters (%zu)",
		    (size_t)boot->NumClusters);
		bitmap_dtor(&usedbitmap);
		releasefat(boot, &buffer);
		free(fat);
		return FSFATAL;
	}

	if (buffer[0] != boot->bpbMedia
	    || buffer[1] != 0xff || buffer[2] != 0xff
	    || (boot->ClustMask == CLUST16_MASK && buffer[3] != 0xff)
	    || (boot->ClustMask == CLUST32_MASK
		&& ((buffer[3]&0x0f) != 0x0f
		    || buffer[4] != 0xff || buffer[5] != 0xff
		    || buffer[6] != 0xff || (buffer[7]&0x0f) != 0x0f))) {

		/* Windows 95 OSR2 (and possibly any later) changes
		 * the FAT signature to 0xXXffff7f for FAT16 and to
		 * 0xXXffff0fffffff07 for FAT32 upon boot, to know that the
		 * file system is dirty if it doesn't reboot cleanly.
		 * Check this special condition before errorring out.
		 */
		if (buffer[0] == boot->bpbMedia && buffer[1] == 0xff
		    && buffer[2] == 0xff
		    && ((boot->ClustMask == CLUST16_MASK && buffer[3] == 0x7f)
			|| (boot->ClustMask == CLUST32_MASK
			    && buffer[3] == 0x0f && buffer[4] == 0xff
			    && buffer[5] == 0xff && buffer[6] == 0xff
			    && buffer[7] == 0x07)))
			ret |= FSDIRTY;
		else {
			/* just some odd byte sequence in FAT */

			switch (boot->ClustMask) {
			case CLUST32_MASK:
				pwarn("%s (%02x%02x%02x%02x%02x%02x%02x%02x)\n",
				      "FAT starts with odd byte sequence",
				      buffer[0], buffer[1], buffer[2], buffer[3],
				      buffer[4], buffer[5], buffer[6], buffer[7]);
				break;
			case CLUST16_MASK:
				pwarn("%s (%02x%02x%02x%02x)\n",
				    "FAT starts with odd byte sequence",
				    buffer[0], buffer[1], buffer[2], buffer[3]);
				break;
			default:
				pwarn("%s (%02x%02x%02x)\n",
				    "FAT starts with odd byte sequence",
				    buffer[0], buffer[1], buffer[2]);
				break;
			}

			if (ask(1, "Correct")) {
				p = buffer;

				*p++ = (u_char)boot->bpbMedia;
				*p++ = 0xff;
				*p++ = 0xff;
				switch (boot->ClustMask) {
				case CLUST16_MASK:
					*p++ = 0xff;
					break;
				case CLUST32_MASK:
					*p++ = 0x0f;
					*p++ = 0xff;
					*p++ = 0xff;
					*p++ = 0xff;
					*p++ = 0x0f;
					break;
				default:
					break;
				}
			}
		}
	}

	fat->fatbuf = buffer;
	fat->boot = boot;

	/* Traverse the FAT table and populate head map */
	for (cl = CLUST_FIRST; cl < boot->NumClusters; cl++) {
		nextcl = fat_get_cl_next(fat, cl);

		/* Check if the next cluster number is valid */
		if (nextcl == CLUST_FREE) {
			fat_clear_cl_head(cl);
			boot->NumFree++;
		} else if (nextcl == CLUST_BAD) {
			fat_clear_cl_head(cl);
			boot->NumBad++;
		} else if (nextcl < CLUST_FIRST ||
		    (nextcl >= boot->NumClusters && nextcl < CLUST_EOFS)) {
			pwarn("Cluster %u continues with %s "
			    "cluster number %u\n",
			    cl, (nextcl < CLUST_RSRVD) ?
				"out of range" : "reserved",
			    nextcl & boot->ClustMask);
			if (ask(0, "Truncate")) {
				fat_set_cl_next(fat, cl, CLUST_EOF);
				ret |= FSFATMOD;
			}
		} else if (nextcl < boot->NumClusters) {
			if (fat_is_cl_head(nextcl)) {
				fat_clear_cl_head(nextcl);
			} else {
				/*
				 * We know cl have crossed another
				 * chain that we have already visited.
				 * Ignore this for now.
				 */
			}
		}

	}

	if (ret & FSFATAL) {
		releasefat(boot, &buffer);
		free(fat);
		*fp = NULL;
	} else
		*fp = fat;
	return ret;
}

/*
 * Get type of reserved cluster
 */
const char *
rsrvdcltype(cl_t cl)
{
	if (cl == CLUST_FREE)
		return "free";
	if (cl < CLUST_BAD)
		return "reserved";
	if (cl > CLUST_BAD)
		return "as EOF";
	return "bad";
}

/*
 * Examine a cluster chain for errors
 */
int
checkchain(struct fat_descriptor *fat, cl_t head, size_t *chainsize)
{
	cl_t current_cl, next_cl;
	struct bootblock *boot = fat_get_boot(fat);

	*chainsize = 0;

	current_cl = head;
	for (current_cl = head; ; current_cl = next_cl) {
		next_cl = fat_get_cl_next(fat, current_cl);
		if (next_cl < CLUST_FIRST || next_cl > boot->NumClusters)
			break;
		if (fat_is_cl_used(next_cl)) {
			/* We have seen this CL in somewhere else */
			pwarn("Cluster %u crossed a chain at %u with %u\n",
			    head, current_cl, next_cl);
			if (ask(0, "Truncate")) {
				fat_set_cl_next(fat, current_cl, CLUST_EOF);
				return FSFATMOD;
			} else {
				return FSERROR;
			}

		}
		(*chainsize)++;
		fat_set_cl_used(next_cl);
	}

	/* A natural end */
	if (next_cl > CLUST_EOFS) {
		(*chainsize)++;
		return FSOK;
	}

	pwarn("Cluster %u continues with %s cluster number %u\n",
	    current_cl,
	    next_cl < CLUST_RSRVD ? "out of range" : "reserved",
	    next_cl & fat->boot->ClustMask);
	if (ask(0, "Truncate")) {
		fat_set_cl_next(fat, current_cl, CLUST_EOF);
		return FSFATMOD;
	} else {
		return FSERROR;
	}
}

/*
 * Clear cluster chain from head.
 */
void
clearchain(struct bootblock *boot, struct fat_descriptor *fat, cl_t head)
{
	cl_t current_cl, next_cl;

	for (current_cl = head;
	    current_cl >= CLUST_FIRST && current_cl < boot->NumClusters;
	    current_cl = next_cl) {
		next_cl = fat_get_cl_next(fat, current_cl);
		fat_set_cl_next(fat, current_cl, CLUST_FREE);
		if (fat_is_cl_used(current_cl)) {
			fat_clear_cl_used(current_cl);
		}
	}
}

/*
 * Write out FAT
 */
int
writefat(int fs, struct bootblock *boot, struct fat_descriptor *fat)
{
	u_int i;
	size_t fatsz;
	off_t off;
	int ret = FSOK;

	fatsz = fat->fatsize;
	for (i = is_mmapped ? 1 : 0; i < boot->bpbFATs; i++) {
		off = boot->bpbResSectors + i * boot->FATsecs;
		off *= boot->bpbBytesPerSec;
		if (lseek(fs, off, SEEK_SET) != off
		    || (size_t)write(fs, fat->fatbuf, fatsz) != fatsz) {
			perr("Unable to write FAT");
			ret = FSFATAL; /* Return immediately?		XXX */
		}
	}

	return ret;
}

/*
 * Check a complete in-memory FAT for lost cluster chains
 */
int
checklost(int dosfs, struct bootblock *boot, struct fat_descriptor *fat)
{
	cl_t head;
	int mod = FSOK;
	int ret;
	size_t chains, chainlength;

	/*
	 * At this point, we have already traversed all directories.
	 * All remaining chain heads in the bitmap are heads of lost
	 * chains.
	 */
	chains = fat_get_head_count();
	for (head = CLUST_FIRST;
	    chains > 0 && head < boot->NumClusters;
	    head++) {
		/*
		 * We expect the bitmap to be very sparse, so skip if
		 * the range is full of 0's
		 */
		if (head % LONG_BIT == 0 && fat_is_cl_head_in_range(head)) {
			head += LONG_BIT;
			if (chains >= LONG_BIT) {
				chains -= LONG_BIT;
			} else {
				chains = 0;
			}
			continue;
		}
		if (fat_is_cl_head(head)) {
			ret = checkchain(fat, head, &chainlength);
			if (ret != FSERROR) {
				pwarn("Lost cluster chain at cluster %u\n"
				    "%zd Cluster(s) lost\n",
				    head, chainlength);
				mod |= ret = reconnect(dosfs, fat, head,
				    chainlength);
			}
			if (mod & FSFATAL)
				break;
			if (ret == FSERROR && ask(0, "Clear")) {
				clearchain(boot, fat, head);
				mod |= FSFATMOD;
			}
		}
		chains--;
	}

	finishlf();

	if (boot->bpbFSInfo) {
		ret = 0;
		if (boot->FSFree != 0xffffffffU &&
		    boot->FSFree != boot->NumFree) {
			pwarn("Free space in FSInfo block (%u) not correct (%u)\n",
			      boot->FSFree, boot->NumFree);
			if (ask(1, "Fix")) {
				boot->FSFree = boot->NumFree;
				ret = 1;
			}
		}
		if (boot->FSNext != 0xffffffffU &&
		    (boot->FSNext >= boot->NumClusters ||
		    (boot->NumFree && fat_get_cl_next(fat, boot->FSNext) != CLUST_FREE))) {
			pwarn("Next free cluster in FSInfo block (%u) %s\n",
			      boot->FSNext,
			      (boot->FSNext >= boot->NumClusters) ? "invalid" : "not free");
			if (ask(1, "fix"))
				for (head = CLUST_FIRST; head < boot->NumClusters; head++)
					if (fat_get_cl_next(fat, head) == CLUST_FREE) {
						boot->FSNext = head;
						ret = 1;
						break;
					}
		}
		if (ret)
			mod |= writefsinfo(dosfs, boot);
	}

	bitmap_dtor(&usedbitmap);
	return mod;
}
