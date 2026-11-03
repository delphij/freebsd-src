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

#ifndef _CTFCONV_ELF_H_
#define _CTFCONV_ELF_H_

#include <libelf.h>

/* A symbol table entry in host form, independent of the ELF class. */
struct elfsym {
	uint64_t	 st_value;
	uint64_t	 st_size;
	uint32_t	 st_name;
	uint16_t	 st_shndx;
	uint8_t		 st_info;
};

struct elfobj {
	const char	*path;
	Elf		*e;
	int		 fd;
	int		 class;		/* ELFCLASS32 or ELFCLASS64 */
	int		 bigendian;
	unsigned int	 type;		/* e_type */
	unsigned int	 machine;	/* e_machine */
	size_t		 shnum, shstrndx;
	size_t		 symidx;	/* .symtab section index, 0 if none */
	size_t		 ctfidx;	/* existing .SUNW_ctf, 0 if none */
	int		 error;		/* a section was malformed (reported) */

	struct elfsym	*syms;
	size_t		 nsyms;
	const char	*strtab;	/* .strtab, in the mapped file */
	size_t		 strtabsz;

	/* Relocated copies of the debug sections. */
	char		*info, *abbrev, *str;
	size_t		 infolen, abbrevlen, strlen;
	char		*addrs, *stroffs;	/* DWARF 5 indirection tables */
	size_t		 addrslen, stroffslen;
};

#define ELF_NODWARF	2

extern uint64_t	*elf_shbase;	/* synthetic section bases, ET_REL */
extern size_t	 elf_nshbase;

int	 elfobj_open(struct elfobj *, const char *, int);
void	 elfobj_close(struct elfobj *);
int	 elfobj_write_ctf(struct elfobj *, int, const char *, const char *,
	     size_t);

#endif /* _CTFCONV_ELF_H_ */
