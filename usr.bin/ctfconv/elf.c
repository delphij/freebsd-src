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

/*
 * ELF access through libelf: any class and byte order, relocations
 * applied to the debug sections of relocatable objects, and in-place
 * insertion of the .SUNW_ctf section.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "elf.h"
#include "word.h"
#include "xmalloc.h"

#define CTF_SECTION	".SUNW_ctf"

/*
 * In a relocatable object, symbol values and relocated addresses are
 * section-relative.  Every allocated section gets a distinct synthetic
 * base so that both live in one address space and can be compared.
 */
uint64_t	*elf_shbase;
size_t		 elf_nshbase;

/*
 * Size of the word an absolute data relocation writes, or 0 for a
 * relocation type that does not occur in debug sections or that we do
 * not handle.  Only ``S + A'' word relocations matter here.
 */
static int
reloc_size(unsigned int machine, unsigned int type)
{
	switch (machine) {
	case EM_X86_64:
		if (type == R_X86_64_64)
			return (8);
		if (type == R_X86_64_32)
			return (4);
		break;
	case EM_386:
		if (type == R_386_32)
			return (4);
		break;
	case EM_AARCH64:
		if (type == R_AARCH64_ABS64)
			return (8);
		if (type == R_AARCH64_ABS32)
			return (4);
		break;
	case EM_ARM:
		if (type == R_ARM_ABS32)
			return (4);
		break;
	case EM_RISCV:
		if (type == R_RISCV_64)
			return (8);
		if (type == R_RISCV_32)
			return (4);
		break;
	case EM_PPC:
		if (type == R_PPC_ADDR32)
			return (4);
		break;
	case EM_PPC64:
		/* R_PPC64_ADDR32 has the number of R_PPC_ADDR32. */
		if (type == R_PPC64_ADDR64)
			return (8);
		if (type == R_PPC_ADDR32)
			return (4);
		break;
	default:
		break;
	}
	return (0);
}

static uint64_t
sym_address(const struct elfsym *sym)
{
	if (sym->st_shndx == SHN_UNDEF || sym->st_shndx >= SHN_LORESERVE ||
	    sym->st_shndx >= elf_nshbase)
		return (sym->st_value);
	return (elf_shbase[sym->st_shndx] + sym->st_value);
}

/*
 * Apply the relocations targeting section ``sidx'' to our copy of its
 * contents.
 */
static void
apply_relocs(struct elfobj *eo, size_t sidx, char *data, size_t len)
{
	Elf_Scn *scn = NULL;
	Elf_Data *d;
	GElf_Shdr sh;
	GElf_Rela rela;
	GElf_Rel rel;
	size_t i, n, rsym;
	uint64_t value, roff;
	int rsize, nwarn = 0;

	while ((scn = elf_nextscn(eo->e, scn)) != NULL) {
		if (gelf_getshdr(scn, &sh) == NULL)
			continue;
		if ((sh.sh_type != SHT_RELA && sh.sh_type != SHT_REL) ||
		    sh.sh_info != sidx || sh.sh_entsize == 0)
			continue;
		if (sh.sh_link != eo->symidx) {
			warnx("%s: relocations against another symbol table",
			    eo->path);
			continue;
		}
		if ((d = elf_getdata(scn, NULL)) == NULL)
			continue;

		n = sh.sh_size / sh.sh_entsize;
		for (i = 0; i < n; i++) {
			unsigned int rtype;
			int64_t addend;

			if (sh.sh_type == SHT_RELA) {
				if (gelf_getrela(d, i, &rela) == NULL)
					break;
				rsym = GELF_R_SYM(rela.r_info);
				rtype = GELF_R_TYPE(rela.r_info);
				roff = rela.r_offset;
				addend = rela.r_addend;
			} else {
				if (gelf_getrel(d, i, &rel) == NULL)
					break;
				rsym = GELF_R_SYM(rel.r_info);
				rtype = GELF_R_TYPE(rel.r_info);
				roff = rel.r_offset;
				addend = 0;
			}

			rsize = reloc_size(eo->machine, rtype);
			if (rsize == 0) {
				if (nwarn++ == 0)
					warnx("%s: unhandled relocation type "
					    "%u for machine %u", eo->path,
					    rtype, eo->machine);
				continue;
			}
			if (rsym >= eo->nsyms || roff + rsize > len)
				continue;

			if (sh.sh_type == SHT_REL)
				addend = (int64_t)load_word(data + roff, rsize,
				    eo->bigendian);
			value = sym_address(&eo->syms[rsym]) + addend;
			store_word(data + roff, rsize, eo->bigendian, value);
		}
	}
}

/*
 * Contents of a section carrying SHF_COMPRESSED: an
 * Elf_Chdr in the file's class and byte order, then a zlib stream that
 * inflates to ch_size bytes.  Userland objects and binaries are built
 * that way (-gz=zlib in DEBUG_FILES_CFLAGS); the kernel is not.  On a
 * malformed or unsupported section the error is reported, eo->error is
 * set and NULL is returned.
 */
static char *
section_inflate(struct elfobj *eo, const char *name, const char *buf,
    size_t len, size_t *plen)
{
	uint64_t type, size;
	size_t hdrlen;
	uLongf dlen;
	char *p;
	int rc;

	/*
	 * Elf64_Chdr: ch_type, ch_reserved, ch_size, ch_addralign;
	 * Elf32_Chdr: ch_type, ch_size, ch_addralign, 32-bit fields.
	 */
	hdrlen = (eo->class == ELFCLASS64) ? 24 : 12;
	if (len < hdrlen) {
		warnx("%s: %s: compression header truncated", eo->path, name);
		eo->error = 1;
		return (NULL);
	}
	type = load_word(buf, 4, eo->bigendian);
	if (eo->class == ELFCLASS64)
		size = load_word(buf + 8, 8, eo->bigendian);
	else
		size = load_word(buf + 4, 4, eo->bigendian);
	if (type != ELFCOMPRESS_ZLIB) {
		warnx("%s: %s: unsupported compression type %llu", eo->path,
		    name, (unsigned long long)type);
		eo->error = 1;
		return (NULL);
	}
	if (size > SIZE_MAX / 2) {
		warnx("%s: %s: compressed section too large", eo->path, name);
		eo->error = 1;
		return (NULL);
	}
	p = xmalloc(size > 0 ? size : 1);
	dlen = size;
	rc = uncompress((Bytef *)p, &dlen, (const Bytef *)buf + hdrlen,
	    len - hdrlen);
	if (rc != Z_OK || dlen != size) {
		warnx("%s: %s: zlib inflate failed: %s", eo->path, name,
		    rc == Z_OK ? "inflated size differs from ch_size" :
		    zError(rc));
		free(p);
		eo->error = 1;
		return (NULL);
	}
	*plen = size;
	return (p);
}

/*
 * Copy of a section's contents, inflated if the section is compressed,
 * relocated if the object is relocatable (relocations address the
 * uncompressed contents).  NULL for an absent or empty section, and
 * also for a malformed one, which sets eo->error.
 */
static char *
section_copy(struct elfobj *eo, size_t sidx, size_t *plen)
{
	Elf_Scn *scn;
	Elf_Data *d;
	GElf_Shdr sh;
	const char *name;
	size_t len;
	char *p;

	if ((scn = elf_getscn(eo->e, sidx)) == NULL ||
	    gelf_getshdr(scn, &sh) == NULL)
		return (NULL);
	if (sh.sh_type == SHT_NOBITS || sh.sh_size == 0)
		return (NULL);
	if ((d = elf_rawdata(scn, NULL)) == NULL || d->d_buf == NULL)
		return (NULL);

	len = d->d_size < sh.sh_size ? d->d_size : sh.sh_size;
	if (sh.sh_flags & SHF_COMPRESSED) {
		name = elf_strptr(eo->e, eo->shstrndx, sh.sh_name);
		p = section_inflate(eo, name != NULL ? name : "?", d->d_buf,
		    len, &len);
		if (p == NULL)
			return (NULL);
	} else {
		/* What the file holds, not what the header claims. */
		p = xmalloc(len);
		memcpy(p, d->d_buf, len);
	}
	if (eo->type == ET_REL)
		apply_relocs(eo, sidx, p, len);
	*plen = len;
	return (p);
}

/*
 * Gather what the converter needs from the file open on ``fd'' (named
 * ``path'' in messages).  The descriptor is owned by ``eo'' from here
 * on.  Returns 0 on success, ELF_NODWARF when the file has no debug
 * information, -1 on error (already reported).
 */
int
elfobj_open(struct elfobj *eo, const char *path, int fd)
{
	GElf_Ehdr eh;
	GElf_Shdr sh;
	Elf_Scn *scn;
	Elf_Data *d;
	const char *name;
	size_t i, infoidx = 0, abbrevidx = 0, stridx = 0;
	size_t addridx = 0, stroffidx = 0;
	uint64_t cur = 0;

	memset(eo, 0, sizeof(*eo));
	eo->path = path;
	eo->fd = fd;

	if (elf_version(EV_CURRENT) == EV_NONE) {
		warnx("libelf: %s", elf_errmsg(-1));
		goto fail;
	}
	if ((eo->e = elf_begin(eo->fd, ELF_C_READ, NULL)) == NULL) {
		warnx("%s: %s", path, elf_errmsg(-1));
		goto fail;
	}
	if (elf_kind(eo->e) != ELF_K_ELF) {
		warnx("%s: not an ELF file", path);
		goto fail;
	}
	if (gelf_getehdr(eo->e, &eh) == NULL) {
		warnx("%s: %s", path, elf_errmsg(-1));
		goto fail;
	}

	eo->class = gelf_getclass(eo->e);
	eo->bigendian = (eh.e_ident[EI_DATA] == ELFDATA2MSB);
	eo->type = eh.e_type;
	eo->machine = eh.e_machine;
	if (eh.e_ident[EI_DATA] != ELFDATA2LSB &&
	    eh.e_ident[EI_DATA] != ELFDATA2MSB) {
		warnx("%s: unknown data encoding %u", path,
		    eh.e_ident[EI_DATA]);
		goto fail;
	}
	if (elf_getshdrnum(eo->e, &eo->shnum) != 0 ||
	    elf_getshdrstrndx(eo->e, &eo->shstrndx) != 0) {
		warnx("%s: %s", path, elf_errmsg(-1));
		goto fail;
	}

	/* Section bases (ET_REL) and the sections we need, by name. */
	elf_nshbase = eo->shnum;
	elf_shbase = xcalloc(eo->shnum > 0 ? eo->shnum : 1,
	    sizeof(*elf_shbase));
	for (i = 1; i < eo->shnum; i++) {
		if ((scn = elf_getscn(eo->e, i)) == NULL ||
		    gelf_getshdr(scn, &sh) == NULL)
			continue;
		if (eo->type == ET_REL && (sh.sh_flags & SHF_ALLOC)) {
			elf_shbase[i] = cur;
			cur = (cur + sh.sh_size + 4096) & ~(uint64_t)4095;
		}
		if (sh.sh_type == SHT_SYMTAB && eo->symidx == 0)
			eo->symidx = i;
		name = elf_strptr(eo->e, eo->shstrndx, sh.sh_name);
		if (name == NULL)
			continue;
		if (strcmp(name, ".debug_info") == 0)
			infoidx = i;
		else if (strcmp(name, ".debug_abbrev") == 0)
			abbrevidx = i;
		else if (strcmp(name, ".debug_str") == 0)
			stridx = i;
		else if (strcmp(name, ".debug_addr") == 0)
			addridx = i;
		else if (strcmp(name, ".debug_str_offsets") == 0)
			stroffidx = i;
		else if (strcmp(name, CTF_SECTION) == 0)
			eo->ctfidx = i;
	}

	/* Symbol table, in host form. */
	if (eo->symidx != 0 && (scn = elf_getscn(eo->e, eo->symidx)) != NULL &&
	    gelf_getshdr(scn, &sh) != NULL && sh.sh_entsize != 0 &&
	    (d = elf_getdata(scn, NULL)) != NULL) {
		GElf_Sym sym;

		eo->nsyms = sh.sh_size / sh.sh_entsize;
		eo->syms = xcalloc(eo->nsyms > 0 ? eo->nsyms : 1,
		    sizeof(*eo->syms));
		for (i = 0; i < eo->nsyms; i++) {
			if (gelf_getsym(d, i, &sym) == NULL)
				break;
			eo->syms[i].st_name = sym.st_name;
			eo->syms[i].st_value = sym.st_value;
			eo->syms[i].st_size = sym.st_size;
			eo->syms[i].st_info = sym.st_info;
			eo->syms[i].st_shndx = sym.st_shndx;
		}
		/* Its string table. */
		if (sh.sh_link != 0 && sh.sh_link < eo->shnum &&
		    (scn = elf_getscn(eo->e, sh.sh_link)) != NULL &&
		    (d = elf_rawdata(scn, NULL)) != NULL) {
			eo->strtab = d->d_buf;
			eo->strtabsz = d->d_size;
		}
	} else
		warnx("%s: no symbol table", path);

	if (infoidx == 0 || abbrevidx == 0)
		return (ELF_NODWARF);

	eo->info = section_copy(eo, infoidx, &eo->infolen);
	eo->abbrev = section_copy(eo, abbrevidx, &eo->abbrevlen);
	if (stridx != 0)
		eo->str = section_copy(eo, stridx, &eo->strlen);
	/* DWARF 5 indirection tables, relocated like the others in a .o. */
	if (addridx != 0)
		eo->addrs = section_copy(eo, addridx, &eo->addrslen);
	if (stroffidx != 0)
		eo->stroffs = section_copy(eo, stroffidx, &eo->stroffslen);
	if (eo->error)
		goto fail;
	if (eo->info == NULL || eo->abbrev == NULL)
		return (ELF_NODWARF);

	return (0);

fail:
	elfobj_close(eo);
	return (-1);
}

void
elfobj_close(struct elfobj *eo)
{
	free(eo->info);
	free(eo->abbrev);
	free(eo->str);
	free(eo->addrs);
	free(eo->stroffs);
	free(eo->syms);
	eo->info = eo->abbrev = eo->str = eo->addrs = eo->stroffs = NULL;
	eo->syms = NULL;
	if (eo->e != NULL)
		elf_end(eo->e);
	eo->e = NULL;
	if (eo->fd != -1)
		close(eo->fd);
	eo->fd = -1;
}

/*
 * Serialize one section header in the file's class and byte order.
 */
static void
put_shdr(char *p, const struct elfobj *eo, const GElf_Shdr *sh)
{
	int be = eo->bigendian;

	if (eo->class == ELFCLASS64) {
		store_word(p + 0, 4, be, sh->sh_name);
		store_word(p + 4, 4, be, sh->sh_type);
		store_word(p + 8, 8, be, sh->sh_flags);
		store_word(p + 16, 8, be, sh->sh_addr);
		store_word(p + 24, 8, be, sh->sh_offset);
		store_word(p + 32, 8, be, sh->sh_size);
		store_word(p + 40, 4, be, sh->sh_link);
		store_word(p + 44, 4, be, sh->sh_info);
		store_word(p + 48, 8, be, sh->sh_addralign);
		store_word(p + 56, 8, be, sh->sh_entsize);
	} else {
		store_word(p + 0, 4, be, sh->sh_name);
		store_word(p + 4, 4, be, sh->sh_type);
		store_word(p + 8, 4, be, sh->sh_flags);
		store_word(p + 12, 4, be, sh->sh_addr);
		store_word(p + 16, 4, be, sh->sh_offset);
		store_word(p + 20, 4, be, sh->sh_size);
		store_word(p + 24, 4, be, sh->sh_link);
		store_word(p + 28, 4, be, sh->sh_info);
		store_word(p + 32, 4, be, sh->sh_addralign);
		store_word(p + 36, 4, be, sh->sh_entsize);
	}
}

static int
write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = write(fd, p, len)) == -1) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		p += n;
		len -= n;
	}
	return (0);
}

/*
 * Add or replace the .SUNW_ctf section.  Every byte of
 * the original file is kept where it is; the section header string
 * table (when it must grow), the new section and a new section header
 * table are appended, and the ELF header is patched to point at the
 * latter.  The result is written to a temporary file in the same
 * directory and renamed over the original, so an error leaves the
 * input untouched.  The directory is reached only through ``dfd'', in
 * which the input is named ``base'': the tool is in capability mode by
 * now (see sandbox() in ctfconv.c).
 */
int
elfobj_write_ctf(struct elfobj *eo, int dfd, const char *base,
    const char *ctf, size_t ctflen)
{
	GElf_Shdr *shdrs, newsh;
	Elf_Scn *scn;
	Elf_Data *d;
	struct stat st;
	char *tmp, *shtab = NULL, *shstr = NULL, *hdr;
	const char *shstrdata;
	size_t i, shstrlen, nameoff, shentsize, newshnum, ctfidx;
	size_t ehsize, hdrlen;
	off_t filesz, pos, shstroff = 0, ctfoff, shoff;
	int fd = -1, be = eo->bigendian, error = -1, need_name;

	if (fstat(eo->fd, &st) == -1) {
		warn("fstat %s", eo->path);
		return (-1);
	}
	filesz = st.st_size;

	if (eo->shnum == 0 || eo->shnum >= SHN_LORESERVE ||
	    eo->shstrndx == 0 || eo->shstrndx >= eo->shnum) {
		warnx("%s: unsupported section header table", eo->path);
		return (-1);
	}

	/* Current section headers. */
	shdrs = xcalloc(eo->shnum + 1, sizeof(*shdrs));
	for (i = 0; i < eo->shnum; i++) {
		if ((scn = elf_getscn(eo->e, i)) == NULL ||
		    gelf_getshdr(scn, &shdrs[i]) == NULL) {
			warnx("%s: %s", eo->path, elf_errmsg(-1));
			goto out;
		}
	}

	/* Section header string table, and the name's offset in it. */
	scn = elf_getscn(eo->e, eo->shstrndx);
	if (scn == NULL || (d = elf_rawdata(scn, NULL)) == NULL ||
	    d->d_buf == NULL) {
		warnx("%s: cannot read section names", eo->path);
		goto out;
	}
	shstrdata = d->d_buf;
	shstrlen = d->d_size;
	need_name = 1;
	nameoff = 0;
	if (eo->ctfidx != 0) {
		nameoff = shdrs[eo->ctfidx].sh_name;
		need_name = 0;
	}

	pos = (filesz + 3) & ~(off_t)3;

	if (need_name) {
		nameoff = shstrlen;
		shstr = xmalloc(shstrlen + sizeof(CTF_SECTION));
		memcpy(shstr, shstrdata, shstrlen);
		memcpy(shstr + shstrlen, CTF_SECTION, sizeof(CTF_SECTION));
		shstroff = pos;
		shdrs[eo->shstrndx].sh_offset = shstroff;
		shdrs[eo->shstrndx].sh_size = shstrlen + sizeof(CTF_SECTION);
		pos = (pos + shstrlen + sizeof(CTF_SECTION) + 3) & ~(off_t)3;
	}

	/* The CTF section itself. */
	ctfoff = pos;
	memset(&newsh, 0, sizeof(newsh));
	newsh.sh_name = nameoff;
	newsh.sh_type = SHT_PROGBITS;
	newsh.sh_flags = 0;
	newsh.sh_addr = 0;
	newsh.sh_offset = ctfoff;
	newsh.sh_size = ctflen;
	newsh.sh_link = eo->symidx;
	newsh.sh_info = 0;
	newsh.sh_addralign = 4;
	newsh.sh_entsize = 0;
	if (eo->ctfidx != 0) {
		ctfidx = eo->ctfidx;
		newshnum = eo->shnum;
	} else {
		ctfidx = eo->shnum;
		newshnum = eo->shnum + 1;
	}
	shdrs[ctfidx] = newsh;
	pos = (pos + ctflen + 7) & ~(off_t)7;

	/* New section header table. */
	shoff = pos;
	shentsize = (eo->class == ELFCLASS64) ? 64 : 40;
	shtab = xcalloc(newshnum, shentsize);
	for (i = 0; i < newshnum; i++)
		put_shdr(shtab + i * shentsize, eo, &shdrs[i]);

	/* Patched ELF header: e_shoff and e_shnum. */
	ehsize = (eo->class == ELFCLASS64) ? 64 : 52;
	if ((off_t)ehsize > filesz) {
		warnx("%s: truncated ELF header", eo->path);
		goto out;
	}
	hdr = xmalloc(ehsize);
	hdrlen = ehsize;
	if (pread(eo->fd, hdr, hdrlen, 0) != (ssize_t)hdrlen) {
		warn("read %s", eo->path);
		free(hdr);
		goto out;
	}
	if (eo->class == ELFCLASS64) {
		store_word(hdr + 40, 8, be, shoff);
		store_word(hdr + 60, 2, be, newshnum);
	} else {
		store_word(hdr + 32, 4, be, shoff);
		store_word(hdr + 48, 2, be, newshnum);
	}

	/* Write: temporary file next to the original. */
	if (asprintf(&tmp, "%s.ctf.XXXXXX", base) == -1) {
		warn("asprintf");
		free(hdr);
		goto out;
	}
	if ((fd = mkostempsat(dfd, tmp, 0, O_CLOEXEC)) == -1) {
		warn("%s: temporary file", eo->path);
		free(hdr);
		free(tmp);
		goto out;
	}
	(void)fchmod(fd, st.st_mode & 07777);

	{
		char buf[64 * 1024];
		off_t off = hdrlen;
		ssize_t n;

		if (write_all(fd, hdr, hdrlen) == -1)
			goto wfail;
		while (off < filesz) {
			n = pread(eo->fd, buf, sizeof(buf), off);
			if (n <= 0) {
				warn("read %s", eo->path);
				goto wfail;
			}
			if (write_all(fd, buf, n) == -1)
				goto wfail;
			off += n;
		}
		/* Padding, appended parts. */
		memset(buf, 0, sizeof(buf));
		if (need_name) {
			if (write_all(fd, buf, shstroff - filesz) == -1 ||
			    write_all(fd, shstr, shstrlen + sizeof(CTF_SECTION))
			    == -1)
				goto wfail;
			off = shstroff + shstrlen + sizeof(CTF_SECTION);
		} else
			off = filesz;
		if (write_all(fd, buf, ctfoff - off) == -1 ||
		    write_all(fd, ctf, ctflen) == -1)
			goto wfail;
		off = ctfoff + ctflen;
		if (write_all(fd, buf, shoff - off) == -1 ||
		    write_all(fd, shtab, newshnum * shentsize) == -1)
			goto wfail;
	}

	if (fsync(fd) == -1 || close(fd) == -1) {
		warn("write %s", tmp);
		fd = -1;
		goto wfail;
	}
	fd = -1;
	if (renameat(dfd, tmp, dfd, base) == -1) {
		warn("rename %s", eo->path);
		goto wfail;
	}
	free(hdr);
	free(tmp);
	error = 0;
	goto out;

wfail:
	if (fd != -1) {
		warn("write %s", tmp);
		close(fd);
	}
	(void)unlinkat(dfd, tmp, 0);
	free(hdr);
	free(tmp);
out:
	free(shstr);
	free(shdrs);
	free(shtab);
	return (error);
}
