/*
 * pac_lower: arm64e code for a processor that has no pointer authentication.
 *
 * Darwin userland 17 and later is arm64e only: its code signs and checks pointers with the instructions of ARMv8.3's
 * PAuth. The cores this port runs on (the SDM845's Cortex-A75 and A55) are ARMv8.2. Of those instructions the ones in
 * the hint space (PACIASP, AUTIBSP, PACIA1716, XPACLRI, BTI ...) are no-operations there, as ARM made them to be;
 * every other one is an undefined instruction. This program replaces each of those, in place and in its own four
 * bytes, by the instruction that does the same without the signature:
 *
 *   PACIA PACIB PACDA PACDB AUTIA AUTIB AUTDA AUTDB (and the Z forms), XPACI, XPACD    NOP: a pointer stays as it is
 *   BRAA BRAB BRAAZ BRABZ                                                              BR   Xn
 *   BLRAA BLRAB BLRAAZ BLRABZ                                                          BLR  Xn
 *   RETAA RETAB                                                                        RET
 *   LDRAA LDRAB Xt, [Xn, #off]                                                         LDR / LDUR Xt, [Xn, #off]
 *   LDRAA LDRAB Xt, [Xn, #off]!                                                        LDR  Xt, [Xn, #off]!
 *   PACGA Xd, Xn, Xm                                                                   EOR  Xd, Xn, Xm, ROR #13
 *
 * Nothing signs, so nothing has a signature to check: all of it is consistent as long as every instruction of the
 * system is lowered, the dynamic linker's included. PACGA (a generic 32-bit code over two values) is compared only
 * with another PACGA of the same values, so any function of the two will do.
 *
 * What does not fit in four bytes -- a load whose offset the plain instruction cannot hold -- is counted and left: the
 * kernel has to emulate those when they trap. So is every other instruction newer than ARMv8.2 (FJCVTZS, ...).
 *
 * What is lowered: the sections of a Mach-O file that hold instructions (S_ATTR_PURE_INSTRUCTIONS or
 * S_ATTR_SOME_INSTRUCTIONS), less the ranges its LC_DATA_IN_CODE names (jump tables: data that could look like any
 * instruction). A universal file: its arm64 slices. The file's signature is not touched and no longer matches it.
 *
 *   pac_lower [-n] [-q] FILE...     -n: count, change nothing    -q: no line per file, only the total
 *   pac_lower [-n] --cache FILE     a dyld shared cache: FILE is its first file (dyld_shared_cache_arm64e), the others
 *                                   (FILE.01, ...) are found beside it; every image's sections of instructions
 *   pac_lower --selftest            the encodings, checked against known ones
 *
 * Written from the ARMv8.3 encodings in ARM's Architecture Reference Manual (DDI 0487), Apple's <mach-o/loader.h> and,
 * for the shared cache, the layout in dyld's <mach-o/dyld_cache_format.h> (apple-oss-distributions/dyld).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the instructions ------------------------------------------------------------------------------------------- */

enum {
	K_NONE, K_PAC_AUT, K_XPAC, K_BR, K_BLR, K_RET, K_LDR, K_PACGA,
	K_LEFT_LDR,             /* a PAuth load that does not fit a plain one: left for the kernel */
	K_LEFT_ERET,            /* ERETAA/ERETAB: the kernel's own, not expected in a program */
	K_COUNT
};

static const char *const klass_name[K_COUNT] = {
	"", "pac/aut", "xpac", "br", "blr", "ret", "ldr", "pacga", "ldr left (offset out of reach)", "eret left"
};

#define INSN_NOP        0xD503201Fu

/* The lowered form of `in` in *out and its class; K_NONE (and *out == in) for an instruction that is not one of these. */
static int
lower_insn(uint32_t in, uint32_t *out)
{
	uint32_t rn = (in >> 5) & 31, rt = in & 31;

	*out = in;
	/* data processing, one source: 1 1 0 11010110 00001 opcode:6 Rn Rd, opcode 0..17 */
	if ((in & 0xFFFF0000u) == 0xDAC10000u) {
		uint32_t opcode = (in >> 10) & 0x3F;

		if (opcode <= 7) {                              /* PACIA .. AUTDB Xd, Xn */
			*out = INSN_NOP;
			return K_PAC_AUT;
		}
		if (opcode <= 15 && rn == 31) {                 /* PACIZA .. AUTDZB Xd */
			*out = INSN_NOP;
			return K_PAC_AUT;
		}
		if ((opcode == 16 || opcode == 17) && rn == 31) { /* XPACI, XPACD Xd */
			*out = INSN_NOP;
			return K_XPAC;
		}
		return K_NONE;
	}
	/* PACGA Xd, Xn, Xm: 1 0 0 11010110 Rm 001100 Rn Rd -> EOR Xd, Xn, Xm, ROR #13 */
	if ((in & 0xFFE0FC00u) == 0x9AC03000u) {
		uint32_t rm = (in >> 16) & 31;

		*out = 0xCAC00000u | (rm << 16) | (13u << 10) | (rn << 5) | rt;
		return K_PACGA;
	}
	/* unconditional branch to a register: 1101011 Z opc:2 11111 0000 A M Rn Rm, here A = 1 (authenticated) */
	if ((in & 0xFEFFF800u) == 0xD61F0800u) {                /* BRAAZ BRABZ (Z = 0, Rm = 31), BRAA BRAB (Z = 1) */
		if (!(in & 0x01000000u) && rt != 31) {
			return K_NONE;
		}
		*out = 0xD61F0000u | (rn << 5);
		return K_BR;
	}
	if ((in & 0xFEFFF800u) == 0xD63F0800u) {                /* BLRAAZ BLRABZ, BLRAA BLRAB */
		if (!(in & 0x01000000u) && rt != 31) {
			return K_NONE;
		}
		*out = 0xD63F0000u | (rn << 5);
		return K_BLR;
	}
	if ((in & 0xFFFFFBFFu) == 0xD65F0BFFu) {                /* RETAA, RETAB */
		*out = 0xD65F03C0u;
		return K_RET;
	}
	if ((in & 0xFFFFFBFFu) == 0xD69F0BFFu) {                /* ERETAA, ERETAB */
		return K_LEFT_ERET;
	}
	/* LDRAA, LDRAB: 11 111 0 00 M S 1 imm9 W 1 Rn Rt; the offset is S:imm9, signed, times 8 */
	if ((in & 0xFF200400u) == 0xF8200400u) {
		int32_t off = (int32_t)((in >> 12) & 0x1FF);
		int writeback = (in >> 11) & 1;

		if (in & 0x00400000u) {
			off -= 512;
		}
		off *= 8;
		if (writeback) {
			if (off < -256 || off > 255) {
				return K_LEFT_LDR;
			}
			*out = 0xF8400C00u | (((uint32_t)off & 0x1FF) << 12) | (rn << 5) | rt;  /* LDR Xt, [Xn, #off]! */
		} else if (off >= 0) {
			*out = 0xF9400000u | ((uint32_t)(off / 8) << 10) | (rn << 5) | rt;      /* LDR Xt, [Xn, #off] */
		} else if (off >= -256) {
			*out = 0xF8400000u | (((uint32_t)off & 0x1FF) << 12) | (rn << 5) | rt;  /* LDUR Xt, [Xn, #off] */
		} else {
			return K_LEFT_LDR;
		}
		return K_LDR;
	}
	return K_NONE;
}

/* ---- a range of instructions ------------------------------------------------------------------------------------ */

static uint64_t total[K_COUNT], total_words, total_files, total_slices;
static int dry_run, quiet;

struct skip {                   /* a range of the file that is data though it lies in a section of instructions */
	uint64_t start, end;
};

static int
skip_compare(const void *a, const void *b)
{
	const struct skip *x = a, *y = b;

	return x->start < y->start ? -1 : x->start > y->start;
}

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* The words of base[start, end), but for the sorted ranges of skips. Offsets are within `base`. */
static void
lower_range(uint8_t *base, uint64_t start, uint64_t end, const struct skip *skips, size_t nskips, uint64_t count[K_COUNT])
{
	size_t s = 0;

	start = (start + 3) & ~(uint64_t)3;
	for (uint64_t at = start; at + 4 <= end; at += 4) {
		uint32_t in, out;
		int klass;

		while (s < nskips && skips[s].end <= at) {
			s++;
		}
		if (s < nskips && skips[s].start < at + 4 && at < skips[s].end) {
			continue;
		}
		in = get32(base + at);
		klass = lower_insn(in, &out);
		count[0]++;
		if (klass == K_NONE) {
			continue;
		}
		count[klass]++;
		if (out != in && !dry_run) {
			put32(base + at, out);
		}
	}
}

/* ---- Mach-O ----------------------------------------------------------------------------------------------------- */

#define MH_MAGIC_64             0xFEEDFACFu
#define FAT_MAGIC               0xCAFEBABEu
#define FAT_MAGIC_64            0xCAFEBABFu
#define CPU_TYPE_ARM64          0x0100000Cu
#define LC_SEGMENT_64           0x19u
#define LC_DATA_IN_CODE         0x29u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
#define SECTION_TYPE            0x000000FFu
#define S_ZEROFILL              0x1u

static uint64_t
get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t
be64(const uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* One 64-bit Mach-O image of `size` bytes at `m`. 0, or -1 if it is not well formed (nothing is changed then). */
static int
lower_macho(uint8_t *m, uint64_t size, uint64_t count[K_COUNT])
{
	uint32_t ncmds, sizeofcmds;
	uint64_t at;
	struct skip *skips = NULL;
	size_t nskips = 0;
	int pass;

	if (size < 32 || get32(m) != MH_MAGIC_64 || get32(m + 4) != CPU_TYPE_ARM64) {
		return -1;
	}
	ncmds = get32(m + 16);
	sizeofcmds = get32(m + 20);
	if ((uint64_t)32 + sizeofcmds > size) {
		return -1;
	}
	/* first the data-in-code ranges, then the sections */
	for (pass = 0; pass < 2; pass++) {
		at = 32;
		for (uint32_t i = 0; i < ncmds; i++) {
			uint32_t cmd, cmdsize;

			if (at + 8 > (uint64_t)32 + sizeofcmds) {
				free(skips);
				return -1;
			}
			cmd = get32(m + at);
			cmdsize = get32(m + at + 4);
			if (cmdsize < 8 || at + cmdsize > (uint64_t)32 + sizeofcmds) {
				free(skips);
				return -1;
			}
			if (pass == 0 && cmd == LC_DATA_IN_CODE && cmdsize >= 16) {
				uint32_t dataoff = get32(m + at + 8), datasize = get32(m + at + 12);

				if ((uint64_t)dataoff + datasize <= size && datasize >= 8) {
					nskips = datasize / 8;
					skips = calloc(nskips, sizeof(*skips));
					if (skips == NULL) {
						return -1;
					}
					for (size_t k = 0; k < nskips; k++) {
						const uint8_t *e = m + dataoff + k * 8;

						skips[k].start = get32(e);
						skips[k].end = skips[k].start + (uint64_t)(e[4] | (e[5] << 8));
					}
					qsort(skips, nskips, sizeof(*skips), skip_compare);
				}
			}
			if (pass == 1 && cmd == LC_SEGMENT_64 && cmdsize >= 72) {
				uint32_t nsects = get32(m + at + 64);

				if ((uint64_t)72 + (uint64_t)nsects * 80 > cmdsize) {
					free(skips);
					return -1;
				}
				for (uint32_t k = 0; k < nsects; k++) {
					const uint8_t *sect = m + at + 72 + (uint64_t)k * 80;
					uint64_t sect_size = get64(sect + 40);
					uint32_t offset = get32(sect + 48), flags = get32(sect + 64);

					if (!(flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) ||
					    (flags & SECTION_TYPE) == S_ZEROFILL || offset == 0 || sect_size == 0) {
						continue;
					}
					if ((uint64_t)offset + sect_size > size) {
						free(skips);
						return -1;
					}
					lower_range(m, offset, (uint64_t)offset + sect_size, skips, nskips, count);
				}
			}
			at += cmdsize;
		}
	}
	free(skips);
	return 0;
}

static int
lower_file(const char *path)
{
	uint64_t count[K_COUNT] = { 0 };
	struct stat st;
	uint8_t *m;
	int fd, slices = 0, bad = 0;

	fd = open(path, dry_run ? O_RDONLY : O_RDWR);
	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "pac_lower: %s: %s\n", path, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	if (st.st_size < 32) {
		close(fd);
		return 1;                       /* not a Mach-O file: nothing to do */
	}
	m = mmap(NULL, (size_t)st.st_size, PROT_READ | (dry_run ? 0 : PROT_WRITE), dry_run ? MAP_PRIVATE : MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		fprintf(stderr, "pac_lower: %s: mmap: %s\n", path, strerror(errno));
		return -1;
	}
	if (get32(m) == MH_MAGIC_64) {
		if (get32(m + 4) == CPU_TYPE_ARM64) {
			bad |= lower_macho(m, (uint64_t)st.st_size, count) < 0;
			slices++;
		}
	} else if (be32(m) == FAT_MAGIC || be32(m) == FAT_MAGIC_64) {
		int wide = be32(m) == FAT_MAGIC_64;
		uint32_t narch = be32(m + 4);
		uint64_t entry = wide ? 32 : 20;

		for (uint32_t i = 0; i < narch && 8 + (uint64_t)(i + 1) * entry <= (uint64_t)st.st_size; i++) {
			const uint8_t *a = m + 8 + (uint64_t)i * entry;
			uint64_t offset = wide ? be64(a + 8) : be32(a + 8), size = wide ? be64(a + 16) : be32(a + 12);

			if (be32(a) != CPU_TYPE_ARM64 || offset + size > (uint64_t)st.st_size || size < 32) {
				continue;
			}
			bad |= lower_macho(m + offset, size, count) < 0;
			slices++;
		}
	}
	if (!dry_run && slices) {
		msync(m, (size_t)st.st_size, MS_SYNC);
	}
	munmap(m, (size_t)st.st_size);
	if (slices == 0) {
		return 1;
	}
	if (bad) {
		fprintf(stderr, "pac_lower: %s: a Mach-O image that is not well formed\n", path);
	}
	if (!quiet) {
		uint64_t changed = 0;

		for (int k = 1; k < K_COUNT; k++) {
			changed += count[k];
		}
		printf("%s: %llu of %llu instructions", path, (unsigned long long)changed, (unsigned long long)count[0]);
		for (int k = 1; k < K_COUNT; k++) {
			if (count[k]) {
				printf(", %s %llu", klass_name[k], (unsigned long long)count[k]);
			}
		}
		printf("\n");
	}
	total_words += count[0];
	for (int k = 1; k < K_COUNT; k++) {
		total[k] += count[k];
	}
	total_files++;
	total_slices += (uint64_t)slices;
	return bad ? -1 : 0;
}

/* ---- the dyld shared cache --------------------------------------------------------------------------------------- */

/*
 * A shared cache is several files (the first, and FILE.01 ... as its header's list of sub-caches names them), each with
 * a header of its own and mappings: (address, size, offset in that file). The images -- every library's Mach-O header,
 * at its address in the cache -- are listed in the first file. An image's sections say where its instructions are, by
 * address; LC_DATA_IN_CODE says which ranges among them are data, by offset from the image's header.
 */
#define CACHE_MAX_FILES         128
#define CACHE_MAX_MAPPINGS      1024
#define LC_SEGMENT_SPLIT_INFO   0x1Eu

struct cache_file {
	char path[1100];
	uint8_t *m;
	uint64_t size;
};

struct cache_mapping {
	uint64_t address, size, offset;
	uint32_t prot;
	int file;
	uint64_t covered;               /* bytes of it that some image's section of instructions named */
};

static struct cache_file cache_files[CACHE_MAX_FILES];
static int cache_nfiles;
static struct cache_mapping cache_mappings[CACHE_MAX_MAPPINGS];
static int cache_nmappings;

static int
cache_open(const char *path)
{
	struct cache_file *f = &cache_files[cache_nfiles];
	struct stat st;
	int fd;

	if (cache_nfiles == CACHE_MAX_FILES) {
		return -1;
	}
	snprintf(f->path, sizeof(f->path), "%s", path);
	fd = open(path, dry_run ? O_RDONLY : O_RDWR);
	if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 0x200) {
		fprintf(stderr, "pac_lower: %s: %s\n", path, fd < 0 ? strerror(errno) : "not a shared cache");
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	f->size = (uint64_t)st.st_size;
	f->m = mmap(NULL, (size_t)f->size, PROT_READ | (dry_run ? 0 : PROT_WRITE), dry_run ? MAP_PRIVATE : MAP_SHARED, fd, 0);
	close(fd);
	if (f->m == MAP_FAILED) {
		fprintf(stderr, "pac_lower: %s: mmap: %s\n", path, strerror(errno));
		return -1;
	}
	if (memcmp(f->m, "dyld_v1 ", 8) != 0) {
		fprintf(stderr, "pac_lower: %s: not a shared cache (no dyld_v1 at its start)\n", path);
		return -1;
	}
	/* its mappings */
	{
		uint32_t off = get32(f->m + 0x10), count = get32(f->m + 0x14);

		if ((uint64_t)off + (uint64_t)count * 32 > f->size) {
			return -1;
		}
		for (uint32_t i = 0; i < count && cache_nmappings < CACHE_MAX_MAPPINGS; i++) {
			const uint8_t *e = f->m + off + (uint64_t)i * 32;
			struct cache_mapping *mp = &cache_mappings[cache_nmappings];

			mp->address = get64(e);
			mp->size = get64(e + 8);
			mp->offset = get64(e + 16);
			mp->prot = get32(e + 28);
			mp->file = cache_nfiles;
			mp->covered = 0;
			if (mp->offset + mp->size > f->size) {
				fprintf(stderr, "pac_lower: %s: a mapping reaches past the file's end\n", path);
				return -1;
			}
			cache_nmappings++;
		}
	}
	return cache_nfiles++;
}

/* Where `size` bytes at the cache's address `address` are: the mapping, or NULL if they are not in one piece of one. */
static struct cache_mapping *
cache_find(uint64_t address, uint64_t size)
{
	for (int i = 0; i < cache_nmappings; i++) {
		struct cache_mapping *mp = &cache_mappings[i];

		if (address >= mp->address && address - mp->address <= mp->size && size <= mp->size - (address - mp->address)) {
			return mp;
		}
	}
	return NULL;
}

static uint8_t *
cache_ptr(uint64_t address, uint64_t size)
{
	struct cache_mapping *mp = cache_find(address, size);

	return mp == NULL ? NULL : cache_files[mp->file].m + mp->offset + (address - mp->address);
}

/* One image, its header at `address`. The counts are added to count[]; -1 if it cannot be read. */
static int
cache_lower_image(uint64_t address, uint64_t count[K_COUNT])
{
	const uint8_t *h = cache_ptr(address, 32), *cmds;
	uint32_t ncmds, sizeofcmds;
	uint64_t at, text_address = address, linkedit_address = 0, linkedit_fileoff = 0;
	uint32_t dic_off = 0, dic_size = 0;
	struct skip *skips = NULL;
	size_t nskips = 0;

	if (h == NULL || get32(h) != MH_MAGIC_64 || get32(h + 4) != CPU_TYPE_ARM64) {
		return -1;
	}
	ncmds = get32(h + 16);
	sizeofcmds = get32(h + 20);
	cmds = cache_ptr(address + 32, sizeofcmds);
	if (cmds == NULL) {
		return -1;
	}
	/* first what the data-in-code table needs: where __TEXT and __LINKEDIT are, and the table's place */
	at = 0;
	for (uint32_t i = 0; i < ncmds && at + 8 <= sizeofcmds; i++) {
		uint32_t cmd = get32(cmds + at), cmdsize = get32(cmds + at + 4);

		if (cmdsize < 8 || at + cmdsize > sizeofcmds) {
			return -1;
		}
		if (cmd == LC_SEGMENT_64 && cmdsize >= 72) {
			if (memcmp(cmds + at + 8, "__TEXT\0", 7) == 0) {
				text_address = get64(cmds + at + 24);
			} else if (memcmp(cmds + at + 8, "__LINKEDIT\0", 11) == 0) {
				linkedit_address = get64(cmds + at + 24);
				linkedit_fileoff = get64(cmds + at + 40);
			}
		} else if (cmd == LC_DATA_IN_CODE && cmdsize >= 16) {
			dic_off = get32(cmds + at + 8);
			dic_size = get32(cmds + at + 12);
		}
		at += cmdsize;
	}
	if (dic_size >= 8 && linkedit_address != 0 && dic_off >= linkedit_fileoff) {
		const uint8_t *table = cache_ptr(linkedit_address + (dic_off - linkedit_fileoff), dic_size);

		if (table != NULL) {
			nskips = dic_size / 8;
			skips = calloc(nskips, sizeof(*skips));
			if (skips == NULL) {
				return -1;
			}
			for (size_t k = 0; k < nskips; k++) {
				/* (kept as addresses here; made offsets into the section's file below) */
				skips[k].start = text_address + get32(table + k * 8);
				skips[k].end = skips[k].start + (uint64_t)(table[k * 8 + 4] | (table[k * 8 + 5] << 8));
			}
			qsort(skips, nskips, sizeof(*skips), skip_compare);
		}
	}
	/* then the sections */
	at = 0;
	for (uint32_t i = 0; i < ncmds && at + 8 <= sizeofcmds; i++) {
		uint32_t cmd = get32(cmds + at), cmdsize = get32(cmds + at + 4);

		if (cmd == LC_SEGMENT_64 && cmdsize >= 72) {
			uint32_t nsects = get32(cmds + at + 64);

			for (uint32_t k = 0; k < nsects && (uint64_t)72 + (uint64_t)(k + 1) * 80 <= cmdsize; k++) {
				const uint8_t *sect = cmds + at + 72 + (uint64_t)k * 80;
				uint64_t sect_address = get64(sect + 32), sect_size = get64(sect + 40);
				uint32_t flags = get32(sect + 64);
				struct cache_mapping *mp;
				struct skip *local = NULL;
				size_t nlocal = 0;
				uint64_t start;

				if (!(flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) ||
				    (flags & SECTION_TYPE) == S_ZEROFILL || sect_size == 0) {
					continue;
				}
				mp = cache_find(sect_address, sect_size);
				if (mp == NULL) {
					free(skips);
					return -1;
				}
				/* the data ranges that fall in this section, as offsets into the mapping's file */
				start = mp->offset + (sect_address - mp->address);
				if (nskips) {
					local = calloc(nskips, sizeof(*local));
					for (size_t d = 0; local != NULL && d < nskips; d++) {
						if (skips[d].end > sect_address && skips[d].start < sect_address + sect_size) {
							local[nlocal].start = start + (skips[d].start > sect_address ? skips[d].start - sect_address : 0);
							local[nlocal].end = start + (skips[d].end - sect_address);
							nlocal++;
						}
					}
				}
				lower_range(cache_files[mp->file].m, start, start + sect_size, local, nlocal, count);
				mp->covered += sect_size;
				free(local);
			}
		}
		at += cmdsize;
	}
	free(skips);
	return 0;
}

static int
lower_cache(const char *path)
{
	uint64_t count[K_COUNT] = { 0 };
	const uint8_t *h;
	uint32_t header_size, images_off, images_count;
	int unreadable = 0;

	if (cache_open(path) < 0) {
		return -1;
	}
	h = cache_files[0].m;
	header_size = get32(h + 0x10);                  /* the mappings follow the header: its size, by the cache's version */
	/* the other files of the cache */
	if (header_size >= 0x190) {
		uint32_t sub_off = get32(h + 0x188), sub_count = get32(h + 0x18C);
		int named = header_size > 0x1C8;        /* entries with the file's suffix (56 bytes); before that ".1", ".2" (24) */
		uint64_t entry = named ? 56 : 24;

		for (uint32_t i = 0; i < sub_count && (uint64_t)sub_off + (uint64_t)(i + 1) * entry <= cache_files[0].size; i++) {
			char sub[1100], suffix[40];

			if (named) {
				memcpy(suffix, h + sub_off + (uint64_t)i * entry + 24, 32);
				suffix[32] = 0;
			} else {
				snprintf(suffix, sizeof(suffix), ".%u", i + 1);
			}
			snprintf(sub, sizeof(sub), "%s%s", path, suffix);
			if (cache_open(sub) < 0) {
				return -1;
			}
		}
	}
	if (header_size > 0x1C0) {
		images_off = get32(h + 0x1C0);
		images_count = get32(h + 0x1C4);
	} else {
		images_off = get32(h + 0x18);
		images_count = get32(h + 0x1C);
	}
	if ((uint64_t)images_off + (uint64_t)images_count * 32 > cache_files[0].size) {
		fprintf(stderr, "pac_lower: %s: the list of images reaches past the file's end\n", path);
		return -1;
	}
	for (uint32_t i = 0; i < images_count; i++) {
		uint64_t address = get64(h + images_off + (uint64_t)i * 32);

		if (cache_lower_image(address, count) < 0) {
			unreadable++;
		}
	}
	if (!dry_run) {
		for (int i = 0; i < cache_nfiles; i++) {
			msync(cache_files[i].m, (size_t)cache_files[i].size, MS_SYNC);
		}
	}
	{
		uint64_t changed = 0, exec_bytes = 0, covered = 0;

		for (int k = 1; k < K_COUNT; k++) {
			changed += count[k];
		}
		for (int i = 0; i < cache_nmappings; i++) {
			if (cache_mappings[i].prot & 4) {       /* VM_PROT_EXECUTE */
				exec_bytes += cache_mappings[i].size;
			}
			covered += cache_mappings[i].covered;
		}
		printf("%s: %d files, %u images (%d unreadable), %llu of %llu instructions%s", path, cache_nfiles, images_count,
		    unreadable, (unsigned long long)changed, (unsigned long long)count[0], dry_run ? " would change" : "");
		for (int k = 1; k < K_COUNT; k++) {
			if (count[k]) {
				printf(", %s %llu", klass_name[k], (unsigned long long)count[k]);
			}
		}
		printf("\n%s: the sections of instructions are %llu MB of the %llu MB mapped as code\n", path,
		    (unsigned long long)(covered >> 20), (unsigned long long)(exec_bytes >> 20));
	}
	return unreadable ? -1 : 0;
}

/* ---- the self-test ---------------------------------------------------------------------------------------------- */

static int
selftest(void)
{
	static const struct { uint32_t in, out; int klass; const char *what; } t[] = {
		{ 0xDAC10020u, INSN_NOP,    K_PAC_AUT, "pacia x0, x1" },
		{ 0xDAC11A30u, INSN_NOP,    K_PAC_AUT, "autda x16, x17" },
		{ 0xDAC123E3u, INSN_NOP,    K_PAC_AUT, "paciza x3" },
		{ 0xDAC13FF0u, INSN_NOP,    K_PAC_AUT, "autdzb x16" },
		{ 0xDAC143E0u, INSN_NOP,    K_XPAC,    "xpaci x0" },
		{ 0xDAC147F1u, INSN_NOP,    K_XPAC,    "xpacd x17" },
		{ 0x9AC23020u, 0xCAC23420u, K_PACGA,   "pacga x0, x1, x2" },
		{ 0xD61F0A1Fu, 0xD61F0200u, K_BR,      "braaz x16" },
		{ 0xD61F0E1Fu, 0xD61F0200u, K_BR,      "brabz x16" },
		{ 0xD71F0A11u, 0xD61F0200u, K_BR,      "braa x16, x17" },
		{ 0xD63F091Fu, 0xD63F0100u, K_BLR,     "blraaz x8" },
		{ 0xD73F0D09u, 0xD63F0100u, K_BLR,     "blrab x8, x9" },
		{ 0xD65F0BFFu, 0xD65F03C0u, K_RET,     "retaa" },
		{ 0xD65F0FFFu, 0xD65F03C0u, K_RET,     "retab" },
		{ 0xF8200420u, 0xF9400020u, K_LDR,     "ldraa x0, [x1]" },
		{ 0xF8201420u, 0xF9400420u, K_LDR,     "ldraa x0, [x1, #8]" },
		{ 0xF87FF420u, 0xF85F8020u, K_LDR,     "ldraa x0, [x1, #-8]" },
		{ 0xF8FFFC62u, 0xF85F8C62u, K_LDR,     "ldrab x2, [x3, #-8]!" },
		{ 0xF8600420u, 0xF8600420u, K_LEFT_LDR, "ldraa x0, [x1, #-4096]" },
		{ 0xF8222C20u, 0xF8222C20u, K_LEFT_LDR, "ldraa x0, [x1, #272]!" },
		/* what must stay as it is */
		{ INSN_NOP,    INSN_NOP,    K_NONE,    "nop" },
		{ 0xD503233Fu, 0xD503233Fu, K_NONE,    "paciasp (a hint)" },
		{ 0xD50323BFu, 0xD50323BFu, K_NONE,    "autiasp (a hint)" },
		{ 0xD503237Fu, 0xD503237Fu, K_NONE,    "pacibsp (a hint)" },
		{ 0xD50320FFu, 0xD50320FFu, K_NONE,    "xpaclri (a hint)" },
		{ 0xD65F03C0u, 0xD65F03C0u, K_NONE,    "ret" },
		{ 0xD63F0100u, 0xD63F0100u, K_NONE,    "blr x8" },
		{ 0xD61F0200u, 0xD61F0200u, K_NONE,    "br x16" },
		{ 0xF9400020u, 0xF9400020u, K_NONE,    "ldr x0, [x1]" },
		{ 0xDAC00020u, 0xDAC00020u, K_NONE,    "rbit x0, x1" },
		{ 0xDAC01020u, 0xDAC01020u, K_NONE,    "clz x0, x1" },
		{ 0xDAC14BE0u, 0xDAC14BE0u, K_NONE,    "(unallocated, opcode 18 of the PAuth group)" },
		{ 0x9AC22820u, 0x9AC22820u, K_NONE,    "asr x0, x1, x2" },
		{ 0xF8616820u, 0xF8616820u, K_NONE,    "ldr x0, [x1, x1]" },
	};
	int failed = 0;

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		uint32_t out;
		int klass = lower_insn(t[i].in, &out);

		if (klass != t[i].klass || out != t[i].out) {
			printf("selftest: %s (%08x): got %08x class %d, want %08x class %d\n", t[i].what, t[i].in, out, klass,
			    t[i].out, t[i].klass);
			failed++;
		}
	}
	printf("selftest: %zu encodings, %d wrong\n", sizeof(t) / sizeof(t[0]), failed);
	return failed != 0;
}

int
main(int argc, char **argv)
{
	int i = 1, status = 0;

	if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
		return selftest();
	}
	int cache = 0;

	for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
		if (strcmp(argv[i], "-n") == 0) {
			dry_run = 1;
		} else if (strcmp(argv[i], "-q") == 0) {
			quiet = 1;
		} else if (strcmp(argv[i], "--cache") == 0) {
			cache = 1;
		} else {
			fprintf(stderr, "usage: pac_lower [-n] [-q] FILE...   |   pac_lower [-n] --cache FILE   |   pac_lower --selftest\n");
			return 2;
		}
	}
	if (i >= argc) {
		fprintf(stderr, "usage: pac_lower [-n] [-q] FILE...   |   pac_lower [-n] --cache FILE   |   pac_lower --selftest\n");
		return 2;
	}
	if (cache) {
		return i + 1 == argc && lower_cache(argv[i]) == 0 ? 0 : 1;
	}
	for (; i < argc; i++) {
		if (lower_file(argv[i]) < 0) {
			status = 1;
		}
	}
	if (total_files != 1 || quiet) {
		uint64_t changed = 0;

		for (int k = 1; k < K_COUNT; k++) {
			changed += total[k];
		}
		printf("total: %llu files, %llu images, %llu of %llu instructions%s", (unsigned long long)total_files,
		    (unsigned long long)total_slices, (unsigned long long)changed, (unsigned long long)total_words,
		    dry_run ? " would change" : "");
		for (int k = 1; k < K_COUNT; k++) {
			if (total[k]) {
				printf(", %s %llu", klass_name[k], (unsigned long long)total[k]);
			}
		}
		printf("\n");
	}
	return status;
}
