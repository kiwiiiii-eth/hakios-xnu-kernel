/*
 * gpt-test: the loader's rules for a new partition table (xnuloader/gpt.c, "oem gptcheck"/"oem gptwrite"), built on a
 * Mac and run over files, so that every case tools/gpt-test.py makes from a phone's own table is decided by the very
 * code the loader runs.
 *
 *   cc -O1 -Wall -o gpt-test tools/gpt-test.c
 *   gpt-test HEAD TAIL STAGED
 *     HEAD, TAIL  the unit's first and last 8 blocks of 4096 bytes (oem readunit), the two copies on the flash
 *     STAGED      what the host would send: a primary header block and the blocks of its entries
 *   prints ALLOW (and what changes) or REFUSE (and why); exit 0 allowed, 1 refused, 2 the files are wrong
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../xnuloader/gpt.c"

#define BS 4096

static uint8_t *
slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;

	if (f == NULL) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*len + BS);
	if (b == NULL || fread(b, 1, *len, f) != *len) {
		fprintf(stderr, "%s: cannot read\n", path);
		exit(2);
	}
	fclose(f);
	return b;
}

static void
say_range(const char *name, const struct gpt_range *was, const struct gpt_range *now)
{
	printf(" %s ", name);
	if (was->present) printf("%llu..%llu", (unsigned long long)was->first, (unsigned long long)was->last);
	else printf("none");
	printf(" -> ");
	if (now->present) printf("%llu..%llu", (unsigned long long)now->first, (unsigned long long)now->last);
	else printf("none");
	printf(";");
}

int
main(int argc, char **argv)
{
	size_t hl, tl, sl;
	uint8_t *head, *tail, *staged, *p_hdr, *b_hdr, *p_entries = NULL, *b_entries = NULL;
	uint64_t disk = 0;
	int p_ok, b_ok;
	struct gpt_change c;
	const char *why;

	if (argc != 4) {
		fprintf(stderr, "usage: gpt-test HEAD TAIL STAGED\n");
		return 2;
	}
	head = slurp(argv[1], &hl);
	tail = slurp(argv[2], &tl);
	staged = slurp(argv[3], &sl);
	if (hl != 8 * BS || tl != 8 * BS || sl < 2 * BS || sl % BS != 0) {
		fprintf(stderr, "gpt-test: HEAD and TAIL are 8 blocks, STAGED whole blocks\n");
		return 2;
	}
	p_hdr = head + BS;
	b_hdr = tail + 7 * BS;
	/* where the unit ends: as either header says (the loader knows it from the device) */
	if (gpt_header_checks(p_hdr, BS)) disk = gpt_alternate_lba(p_hdr) + 1;
	else if (gpt_header_checks(b_hdr, BS)) disk = gpt_my_lba(b_hdr) + 1;
	p_ok = gpt_header_checks(p_hdr, BS) && gpt_entries_lba(p_hdr) + gpt_entries_blocks_bytes(p_hdr, BS) / BS <= 8;
	if (p_ok) {
		p_entries = head + gpt_entries_lba(p_hdr) * BS;
		p_ok = gpt_table_checks(p_hdr, p_entries, BS);
	}
	b_ok = disk != 0 && gpt_header_checks(b_hdr, BS) && gpt_my_lba(b_hdr) == disk - 1 &&
	    gpt_entries_lba(b_hdr) >= disk - 8 && gpt_entries_lba(b_hdr) < disk - 1;
	if (b_ok) {
		b_entries = tail + (gpt_entries_lba(b_hdr) - (disk - 8)) * BS;
		b_ok = gpt_table_checks(b_hdr, b_entries, BS);
	}
	if (!gpt_header_checks(staged, BS) || BS + gpt_entries_blocks_bytes(staged, BS) != sl) {
		printf("REFUSE: the staged table is not a header and its entries (%zu bytes)\n", sl);
		return 1;
	}
	why = gpt_decide(p_hdr, p_entries ? p_entries : head, p_ok, b_hdr, b_entries ? b_entries : tail, b_ok, staged,
	    staged + BS, BS, &c);
	if (why != NULL) {
		printf("REFUSE: %s (primary %s, backup %s)\n", why, p_ok ? "checks" : "does not check",
		    b_ok ? "checks" : "does not check");
		return 1;
	}
	printf("ALLOW:");
	say_range("userdata", &c.userdata_was, &c.userdata);
	say_range("darwin", &c.darwin_was, &c.darwin);
	printf(" same=%d mend=%d", c.same, c.mend);
	{
		/* the backup the loader would write for it, as its CRC: the host compares it with a real backup */
		static uint8_t hdr[BS], backup[BS];
		uint64_t entries_lba = b_ok ? gpt_entries_lba(b_hdr) : disk - 1 - gpt_entries_blocks_bytes(staged, BS) / BS;

		memcpy(hdr, staged, gpt_header_size(staged));
		gpt_backup_header(hdr, backup, BS, disk - 1, entries_lba);
		printf(" backup_crc=%08x\n", gpt_get32(backup, 16));
	}
	return 0;
}
