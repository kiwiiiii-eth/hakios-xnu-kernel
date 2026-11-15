/*
 * gpt.c: what a new partition table must keep before the loader writes it over the one of the logical unit that holds
 * Android's userdata ("oem gptcheck", "oem gptwrite"). Beta 0.4's install.sh carves the partition darwin off the end
 * of userdata with it, and uninstall.sh puts the phone's own table back; nothing else is to be done with it.
 *
 * Pure functions over buffers, no firmware call: loader.c includes this file, and tools/gpt-test.c builds the same file
 * on a Mac and runs it over tables made from a phone's own (tools/gpt-test.py). The includer gives uint8_t, uint32_t,
 * uint64_t, memcmp, memcpy and memset.
 *
 * The staged table is a primary copy: its header block and the blocks of its entries. It is held against each copy of
 * the table on the flash whose CRCs check (both, when both do: a write cut short earlier leaves the primary new and the
 * backup old, and the staged table must be right against either, so that writing it mends the pair), and against a
 * backup the way it would read as a primary (its own place and the primary's swapped, its entries at the primary's).
 * It is refused unless
 *
 *   - its own CRCs check, and its header is the phone's byte for byte but for the two CRCs: the same places, the same
 *     usable range, the same disk GUID, as many entries of the same size;
 *   - entry by entry, each is the phone's byte for byte, except
 *       the entry of userdata (exactly one in each, at the same index), whose end alone may move, with
 *       start <= end <= the last usable block and 4 GiB at least left to it, and
 *       the entry of darwin (one at most in each): an empty one may become darwin, darwin may become empty, and a
 *       darwin in both is left as it is (moved or resized, what lies in it -- /private/var -- would be lost);
 *   - a darwin there is: of the APFS container type, after userdata's end, from a 2 MiB boundary, within the usable
 *     range, 12 GiB at least (the system image's 7.79 GiB and the two 2 GiB slots of /private/var past 8 GiB);
 *   - no two named entries share a name (the 6T has entries with a type and no name or range: those are left be),
 *     and no other entry overlaps userdata or darwin.
 *
 * So the one thing a table that passes can do is move userdata's end and add, take away or keep darwin: no other
 * partition can be moved, renamed into darwin, dropped or given a new range.
 */

#define GPT_ENTRIES_MAX   (64 * 1024)
#define GPT_MIN_USERDATA  (4ULL << 30)
#define GPT_MIN_DARWIN    (12ULL << 30)
#define GPT_ALIGN_BYTES   (2ULL << 20)

/* the APFS container's type, 7c3457ef-0000-11aa-aa11-00306543ecac, as a GPT holds it */
static const uint8_t gpt_apfs_type[16] = {
	0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac,
};

struct gpt_range {
	int present;
	uint64_t first, last;
};

struct gpt_change {
	struct gpt_range userdata_was, userdata, darwin_was, darwin;
	int same;       /* the staged table is the phone's (both copies), CRCs and all: nothing to write */
	int mend;       /* the copies on the flash differ, or one does not check: a write makes them one again */
};

static uint32_t
gpt_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

static uint64_t
gpt_get64(const uint8_t *p, unsigned at)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--) {
		v = v << 8 | p[at + i];
	}
	return v;
}

static uint32_t
gpt_get32(const uint8_t *p, unsigned at)
{
	return (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24;
}

static void
gpt_put64(uint8_t *p, unsigned at, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		p[at + i] = (uint8_t)(v >> (8 * i));
	}
}

static void
gpt_put32(uint8_t *p, unsigned at, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[at + i] = (uint8_t)(v >> (8 * i));
	}
}

static uint32_t gpt_header_size(const uint8_t *h) { return gpt_get32(h, 12); }
static uint64_t gpt_my_lba(const uint8_t *h) { return gpt_get64(h, 24); }
static uint64_t gpt_alternate_lba(const uint8_t *h) { return gpt_get64(h, 32); }
static uint64_t gpt_last_usable(const uint8_t *h) { return gpt_get64(h, 48); }
static uint64_t gpt_entries_lba(const uint8_t *h) { return gpt_get64(h, 72); }
static uint32_t gpt_count(const uint8_t *h) { return gpt_get32(h, 80); }
static uint32_t gpt_entry_size(const uint8_t *h) { return gpt_get32(h, 84); }
static uint64_t gpt_entries_bytes(const uint8_t *h) { return (uint64_t)gpt_count(h) * gpt_entry_size(h); }

/* the bytes the entries take on the flash, in whole blocks */
static uint64_t
gpt_entries_blocks_bytes(const uint8_t *h, uint64_t bs)
{
	return (gpt_entries_bytes(h) + bs - 1) / bs * bs;
}

/* the header's own CRC checks, and its fields are ones this code can hold: 1 */
static int
gpt_header_checks(const uint8_t *h, uint64_t bs)
{
	uint8_t copy[512];
	uint32_t size = gpt_header_size(h);

	if (memcmp(h, "EFI PART", 8) != 0 || size < 92 || size > sizeof(copy) || size > bs || gpt_entry_size(h) < 128 ||
	    gpt_entry_size(h) % 8 != 0 || gpt_count(h) == 0 || gpt_entries_bytes(h) > GPT_ENTRIES_MAX) {
		return 0;
	}
	memcpy(copy, h, size);
	gpt_put32(copy, 16, 0);
	return gpt_crc32(copy, size) == gpt_get32(h, 16);
}

/* a header that checks, and its entries' CRC as well: 1 */
static int
gpt_table_checks(const uint8_t *h, const uint8_t *entries, uint64_t bs)
{
	return gpt_header_checks(h, bs) && gpt_crc32(entries, gpt_entries_bytes(h)) == gpt_get32(h, 88);
}

static int
gpt_empty(const uint8_t *e)
{
	for (int i = 0; i < 16; i++) {
		if (e[i] != 0) {
			return 0;
		}
	}
	return 1;
}

/* the entry's name is name (ASCII), as UTF-16 and ending there */
static int
gpt_named(const uint8_t *e, const char *name)
{
	unsigned k = 0;

	for (; name[k] != 0; k++) {
		if (k >= 36 || e[56 + 2 * k] != (uint8_t)name[k] || e[57 + 2 * k] != 0) {
			return 0;
		}
	}
	return k == 36 || (e[56 + 2 * k] == 0 && e[57 + 2 * k] == 0);
}

static int
gpt_has_name(const uint8_t *e)
{
	return e[56] != 0 || e[57] != 0;
}

/* the one index of a non-empty entry named name: >= 0; -1 for none, -2 for more than one */
static int
gpt_index(const uint8_t *h, const uint8_t *entries, const char *name)
{
	int found = -1;

	for (uint32_t i = 0; i < gpt_count(h); i++) {
		const uint8_t *e = entries + (uint64_t)i * gpt_entry_size(h);

		if (!gpt_empty(e) && gpt_named(e, name)) {
			if (found != -1) {
				return -2;
			}
			found = (int)i;
		}
	}
	return found;
}

static void
gpt_range_of(const uint8_t *e, struct gpt_range *r)
{
	r->present = 1;
	r->first = gpt_get64(e, 32);
	r->last = gpt_get64(e, 40);
}

static int
gpt_overlap(uint64_t a_first, uint64_t a_last, uint64_t b_first, uint64_t b_last)
{
	return a_first <= b_last && b_first <= a_last;
}

/*
 * The staged table (new_hdr, a primary's, and its entries) against one copy of the phone's (ref_hdr, either copy, and
 * its entries, both checked by the caller; for a backup, primary_entries_lba is where the primary's entries are):
 * NULL when it may be written, else why not. c says what would change.
 */
static const char *
gpt_rules(const uint8_t *ref_hdr, const uint8_t *ref_entries, const uint8_t *new_hdr, const uint8_t *new_entries,
    uint64_t bs, uint64_t primary_entries_lba, struct gpt_change *c)
{
	uint8_t want[512];
	uint32_t size = gpt_header_size(ref_hdr), es, count;
	int ud, ud_was, dw, dw_was;
	const uint8_t *u;

	memset(c, 0, sizeof(*c));
	if (!gpt_table_checks(new_hdr, new_entries, bs)) {
		return "the staged table's CRCs do not check";
	}
	if (gpt_my_lba(new_hdr) != 1) {
		return "the staged header is not a primary one";
	}
	/* the phone's header as the primary would hold it */
	if (size > sizeof(want) || gpt_header_size(new_hdr) != size) {
		return "the staged header is not the size of the phone's";
	}
	memcpy(want, ref_hdr, size);
	if (gpt_my_lba(ref_hdr) != 1) {
		gpt_put64(want, 24, gpt_alternate_lba(ref_hdr));
		gpt_put64(want, 32, gpt_my_lba(ref_hdr));
		gpt_put64(want, 72, primary_entries_lba);
	}
	for (uint32_t i = 0; i < size; i++) {
		if ((i >= 16 && i < 20) || (i >= 88 && i < 92)) {
			continue;
		}
		if (want[i] != new_hdr[i]) {
			return "the staged header differs from the phone's in more than its CRCs";
		}
	}
	es = gpt_entry_size(new_hdr);
	count = gpt_count(new_hdr);
	ud_was = gpt_index(ref_hdr, ref_entries, "userdata");
	ud = gpt_index(new_hdr, new_entries, "userdata");
	if (ud_was < 0 || ud != ud_was) {
		return "not one userdata in each table, at the same entry";
	}
	dw_was = gpt_index(ref_hdr, ref_entries, "darwin");
	dw = gpt_index(new_hdr, new_entries, "darwin");
	if (dw_was == -2 || dw == -2) {
		return "more than one darwin";
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *was = ref_entries + (uint64_t)i * es, *now = new_entries + (uint64_t)i * es;

		if (memcmp(was, now, es) == 0) {
			continue;
		}
		if ((int)i == ud) {
			/* userdata: its end alone */
			if (memcmp(was, now, 40) != 0 || memcmp(was + 48, now + 48, es - 48) != 0) {
				return "userdata changes in more than its end";
			}
			continue;
		}
		/* darwin's entry: empty or darwin before, darwin or empty after, and darwin on one side at least */
		if (!(((int)i == dw_was || gpt_empty(was)) && ((int)i == dw || gpt_empty(now)) && ((int)i == dw_was || (int)i == dw))) {
			return "an entry other than userdata and darwin changes";
		}
		if (dw_was >= 0 && dw >= 0) {
			return "darwin would move or change: it is only added or taken away";
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *a = new_entries + (uint64_t)i * es;

		if (gpt_empty(a) || !gpt_has_name(a)) {
			continue;
		}
		for (uint32_t j = i + 1; j < count; j++) {
			const uint8_t *b = new_entries + (uint64_t)j * es;

			if (!gpt_empty(b) && memcmp(a + 56, b + 56, 72) == 0) {
				return "two entries have the same name";
			}
		}
	}
	u = new_entries + (uint64_t)ud * es;
	gpt_range_of(ref_entries + (uint64_t)ud * es, &c->userdata_was);
	gpt_range_of(u, &c->userdata);
	if (c->userdata.last < c->userdata.first || c->userdata.last > gpt_last_usable(new_hdr)) {
		return "userdata would end outside its unit's usable blocks";
	}
	if ((c->userdata.last - c->userdata.first + 1) * bs < GPT_MIN_USERDATA) {
		return "userdata would be smaller than 4 GiB";
	}
	if (dw_was >= 0) {
		gpt_range_of(ref_entries + (uint64_t)dw_was * es, &c->darwin_was);
	}
	if (dw >= 0) {
		const uint8_t *d = new_entries + (uint64_t)dw * es;

		gpt_range_of(d, &c->darwin);
		if (memcmp(d, gpt_apfs_type, 16) != 0) {
			return "darwin is not of the APFS container type";
		}
		if (c->darwin.first <= c->userdata.last || c->darwin.last < c->darwin.first ||
		    c->darwin.last > gpt_last_usable(new_hdr)) {
			return "darwin is not between userdata's end and the last usable block";
		}
		if ((c->darwin.first * bs) % GPT_ALIGN_BYTES != 0) {
			return "darwin does not begin on a 2 MiB boundary";
		}
		if ((c->darwin.last - c->darwin.first + 1) * bs < GPT_MIN_DARWIN) {
			return "darwin would be smaller than 12 GiB";
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = new_entries + (uint64_t)i * es;
		uint64_t first = gpt_get64(e, 32), last = gpt_get64(e, 40);

		if (gpt_empty(e)) {
			continue;
		}
		if ((int)i != ud && gpt_overlap(first, last, c->userdata.first, c->userdata.last)) {
			return "another entry overlaps userdata";
		}
		if (dw >= 0 && (int)i != dw && gpt_overlap(first, last, c->darwin.first, c->darwin.last)) {
			return "another entry overlaps darwin";
		}
	}
	return NULL;
}

/*
 * The staged table against the phone's two copies (p: the primary, b: the backup; *_ok: its CRCs, header and entries,
 * check): NULL when it may be written, else why not; c says what would change, and whether anything would.
 */
static const char *
gpt_decide(const uint8_t *p_hdr, const uint8_t *p_entries, int p_ok, const uint8_t *b_hdr, const uint8_t *b_entries,
    int b_ok, const uint8_t *new_hdr, const uint8_t *new_entries, uint64_t bs, struct gpt_change *c)
{
	struct gpt_change other;
	const char *why;
	/* where the primary's entries are: as it says, or, when it does not check, where a GPT has them (LBA 2) */
	uint64_t pe = p_ok ? gpt_entries_lba(p_hdr) : 2;

	if (!p_ok && !b_ok) {
		return "neither copy of the phone's table checks";
	}
	if (p_ok && (why = gpt_rules(p_hdr, p_entries, new_hdr, new_entries, bs, pe, c)) != NULL) {
		return why;
	}
	if (b_ok && (why = gpt_rules(b_hdr, b_entries, new_hdr, new_entries, bs, pe, p_ok ? &other : c)) != NULL) {
		return why;
	}
	c->mend = !p_ok || !b_ok || gpt_count(p_hdr) != gpt_count(b_hdr) || gpt_entry_size(p_hdr) != gpt_entry_size(b_hdr) ||
	    memcmp(p_entries, b_entries, gpt_entries_bytes(p_hdr)) != 0;
	c->same = !c->mend && memcmp(p_hdr, new_hdr, gpt_header_size(p_hdr)) == 0 &&
	    memcmp(p_entries, new_entries, gpt_entries_bytes(p_hdr)) == 0;
	return NULL;
}

/* the backup header for a staged primary: the same, at backup_lba with its entries at entries_lba, CRC made again */
static void
gpt_backup_header(const uint8_t *new_hdr, uint8_t *out, uint64_t bs, uint64_t backup_lba, uint64_t entries_lba)
{
	memcpy(out, new_hdr, bs);
	gpt_put64(out, 24, backup_lba);
	gpt_put64(out, 32, 1);
	gpt_put64(out, 72, entries_lba);
	gpt_put32(out, 16, 0);
	gpt_put32(out, 16, gpt_crc32(out, gpt_header_size(out)));
}
