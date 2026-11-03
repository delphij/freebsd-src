/*
 * Copyright (c) 2026 Xin Li <delphij@FreeBSD.org>
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

#ifndef _CTFCONV_WORD_H_
#define _CTFCONV_WORD_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Words of 1 to 8 bytes in a given byte order, for the ELF, DWARF and
 * CTF layers alike: the input is read and the container written in the
 * target's order, whatever the host's.
 */

static inline int
host_bigendian(void)
{
	const uint16_t one = 1;

	return (*(const uint8_t *)&one == 0);
}

static inline uint64_t
load_word(const char *p, size_t size, int bigendian)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < size; i++) {
		if (bigendian)
			v = (v << 8) | (uint8_t)p[i];
		else
			v |= (uint64_t)(uint8_t)p[i] << (8 * i);
	}
	return (v);
}

static inline void
store_word(char *p, size_t size, int bigendian, uint64_t v)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (bigendian)
			p[size - 1 - i] = (char)(v >> (8 * i));
		else
			p[i] = (char)(v >> (8 * i));
	}
}

#endif /* _CTFCONV_WORD_H_ */
