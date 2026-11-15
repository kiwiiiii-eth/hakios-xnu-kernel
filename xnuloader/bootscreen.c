/*
 * bootscreen.c: what a phone that boots HakiOS by itself shows while the loader reads the system image from darwin
 * (about 17 seconds on the 6T) -- before, U-Boot's console over what ABL had left on the screen, cut by the corners.
 *
 *   the clean screen (the default): black, "HakiOS", a progress bar, and one line: "Loading the system · 1.2 / 2.7 GB",
 *   then "Checking the system" while the image's FNV-1a is taken, "Starting HakiOS" as the kernel is started (and
 *   stays until Darwin draws), or "The system could not be loaded" (and the way back to ABL's fastboot)
 *   bootscreen=log (the payload's command line; mk-installimg.sh DEBUG=1): the same, and the loader's last lines of log
 *   at the bottom, dim, as they come
 *   bootscreen=0: nothing drawn
 *
 * Only on the one-shot path that reads the system image from darwin (an installed phone, or a one-shot test): a run from
 * a host over USB draws nothing here. Text through UEFI's console is kept off the screen while it shows (screen_quiet):
 * under a host's U-Boot that console is the screen.
 *
 * Pictures: tools/mkbootscreen.py (Inter, SIL OFL 1.1), as 8-bit coverage drawn in grey over black; every pixel is
 * grey, so the order of the framebuffer's channels does not matter. Places scale with the screen's height (the 6T's
 * 2340); the corners and the notch are left clear.
 *
 * Included by loader.c after SCREEN, print and log_buf.
 */
#include "bootscreen_assets.h"

static int bs_mode;                     /* 0: off; 1: the clean screen; 2: and the log */
static uint32_t bs_shown = 0xffffffff;  /* the progress last drawn, in thousandths; and the phase */
static int bs_phase;

static void
bs_put(int32_t x, int32_t y, uint32_t grey)
{
	if (x < 0 || y < 0 || (uint32_t)x >= SCREEN.width || (uint32_t)y >= SCREEN.height) {
		return;
	}
	((volatile uint32_t *)(uintptr_t)(SCREEN.base + (uint64_t)y * SCREEN.stride))[x] =
	    0xff000000u | grey << 16 | grey << 8 | grey;
}

static void
bs_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t grey)
{
	for (int32_t j = 0; j < h; j++) {
		for (int32_t i = 0; i < w; i++) {
			bs_put(x + i, y + j, grey);
		}
	}
}

/* picture idx, its pen at (x, y) (y: the line's top), in grey level over black */
static void
bs_draw(int idx, int32_t x, int32_t y, uint32_t level)
{
	const struct bs_picture *p = &bs_pictures[idx];
	const uint8_t *r = bs_pool + p->at, *end = bs_pool + p->at + p->bytes;
	uint32_t i = 0, n = (uint32_t)p->w * p->h;

	for (; r + 1 < end && i < n; r += 2) {
		uint32_t count = r[0], v = r[1];

		if (v != 0) {
			for (uint32_t k = 0; k < count && i + k < n; k++) {
				bs_put(x + p->left + (int32_t)((i + k) % p->w), y + p->top + (int32_t)((i + k) / p->w), v * level / 255);
			}
		}
		i += count;
	}
}

/* the picture of a character of BS_GLYPHS (the lines' numbers), or -1 */
static int
bs_glyph(char c)
{
	static const char glyphs[] = BS_GLYPHS;

	for (int i = 0; glyphs[i] != 0; i++) {
		if (glyphs[i] == c) {
			return BS_GLYPH_0 + i;
		}
	}
	return -1;
}

static int32_t
bs_width(const char *s)
{
	int32_t w = 0;

	for (; *s; s++) {
		int g = bs_glyph(*s);

		w += g < 0 ? 0 : bs_pictures[g].advance;
	}
	return w;
}

static int32_t
bs_string(const char *s, int32_t x, int32_t y, uint32_t level)
{
	for (; *s; s++) {
		int g = bs_glyph(*s);

		if (g >= 0) {
			bs_draw(g, x, y, level);
			x += bs_pictures[g].advance;
		}
	}
	return x;
}

/* the screen's places: the 6T's at 2340 high, scaled */
static int32_t bs_y(int32_t at2340) { return (int32_t)((int64_t)at2340 * SCREEN.height / 2340); }
static int32_t bs_x(int32_t at1080) { return (int32_t)((int64_t)at1080 * SCREEN.width / 1080); }

/* the bar: a track, and the part done, its ends rounded */
static void
bs_bar(uint32_t permille)
{
	int32_t w = bs_x(520), h = 10, x = ((int32_t)SCREEN.width - w) / 2, y = bs_y(1230), r = h / 2;
	int32_t done = (int32_t)((int64_t)w * permille / 1000);

	for (int32_t j = 0; j < h; j++) {
		/* the inset of a round end on this row: r - sqrt(r^2 - (d/2)^2), d the row's middle from the bar's in half
		 * pixels; in quarter pixels q = sqrt(16 r^2 - 4 d^2), rounded to whole ones */
		int32_t d = 2 * j + 1 - h, q = 0, inset;

		while ((q + 1) * (q + 1) <= 16 * r * r - 4 * d * d) {
			q++;
		}
		inset = r - (q + 2) / 4;
		for (int32_t i = inset; i < w - inset; i++) {
			bs_put(x + i, y + j, i < done ? 255 : 44);
		}
	}
}

/* "phrase", and "  ·  a.b / c.d GB" when total is given, centred under the bar */
static void
bs_status(int phrase, uint64_t done, uint64_t total)
{
	char numbers[40];
	int n = 0;
	int32_t width = bs_pictures[phrase].advance, x, y = bs_y(1280);

	if (total != 0) {
		uint64_t a = (done + 50000000) / 100000000, b = (total + 50000000) / 100000000;     /* tenths of a GB */
		uint64_t parts[2] = { a, b };

		numbers[n++] = ' ', numbers[n++] = ' ', numbers[n++] = 0x7f, numbers[n++] = ' ', numbers[n++] = ' ';
		for (int k = 0; k < 2; k++) {
			char digits[20];
			int m = 0;
			uint64_t whole = parts[k] / 10;

			do {
				digits[m++] = (char)('0' + whole % 10);
				whole /= 10;
			} while (whole > 0);
			while (m > 0) {
				numbers[n++] = digits[--m];
			}
			numbers[n++] = '.';
			numbers[n++] = (char)('0' + parts[k] % 10);
			if (k == 0) {
				numbers[n++] = ' ', numbers[n++] = '/', numbers[n++] = ' ';
			}
		}
		numbers[n++] = ' ', numbers[n++] = 'G', numbers[n++] = 'B';
	}
	numbers[n] = 0;
	width += bs_width(numbers);
	bs_fill(0, y - 4, (int32_t)SCREEN.width, BS_LINE_HEIGHT + 8, 0);
	x = ((int32_t)SCREEN.width - width) / 2;
	bs_draw(phrase, x, y, 142);
	bs_string(numbers, x + bs_pictures[phrase].advance, y, 142);
}

/* bootscreen=log: the loader's last lines, dim, above the bottom corners */
static void
bs_log(void)
{
	enum { LINES = 7 };
	int32_t top = (int32_t)SCREEN.height - bs_y(150) - LINES * BS_LOG_HEIGHT, left = bs_x(90),
	    right = (int32_t)SCREEN.width - bs_x(90);
	uint32_t starts[LINES], count = 0, end = log_len;

	if (bs_mode != 2) {
		return;
	}
	/* the starts of the last LINES lines (a line not yet ended counts too) */
	while (end > 0 && log_buf[end - 1] == '\n') {
		end--;
	}
	for (uint32_t i = end; i > 0 && count < LINES; i--) {
		if (i == 1 || log_buf[i - 2] == '\n') {
			starts[count++] = i - 1;
		}
	}
	bs_fill(0, top, (int32_t)SCREEN.width, LINES * BS_LOG_HEIGHT, 0);
	for (uint32_t k = 0; k < count; k++) {
		uint32_t at = starts[count - 1 - k];
		int32_t x = left, y = top + (int32_t)(LINES - count + k) * BS_LOG_HEIGHT;

		for (; at < end && log_buf[at] != '\n'; at++) {
			char c = log_buf[at];
			int idx = BS_ASCII_FIRST + ((c >= 32 && c < 127 ? c : '?') - 32);

			if (x + bs_pictures[idx].advance > right) {
				break;
			}
			bs_draw(idx, x, y, 92);
			x += bs_pictures[idx].advance;
		}
	}
}

/* is word (name=value) among the payload's command line? */
static int
bs_cmdline_word(const char *cmdline, const char *word)
{
	size_t n = str_len(word), k;

	for (const char *p = cmdline; p != NULL && *p; ) {
		while (*p == ' ') {
			p++;
		}
		for (k = 0; k < n && p[k] == word[k]; k++) {
		}
		if (k == n && (p[n] == 0 || p[n] == ' ')) {
			return 1;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return 0;
}

/* which screen, from the payload's command line; and the screen cleared, the name, the empty bar */
static void
bootscreen_begin(const char *cmdline, uint64_t total)
{
	bs_mode = SCREEN.base == 0 || bs_cmdline_word(cmdline, "bootscreen=0") ? 0 :
	    bs_cmdline_word(cmdline, "bootscreen=log") ? 2 : 1;
	if (bs_mode == 0) {
		return;
	}
	screen_quiet = 1;
	for (uint32_t y = 0; y < SCREEN.height; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)(SCREEN.base + (uint64_t)y * SCREEN.stride);

		for (uint32_t x = 0; x < SCREEN.width; x++) {
			row[x] = 0xff000000u;
		}
	}
	bs_draw(BS_LOGO, ((int32_t)SCREEN.width - bs_pictures[BS_LOGO].advance) / 2, bs_y(980), 255);
	bs_bar(0);
	bs_status(BS_LOADING, 0, total);
	bs_log();
	bs_shown = 0;
	bs_phase = 0;
}

/* how far: reading (phase 0, the bar's first 85%) or checking (phase 1, the rest); redrawn when it moved a half percent */
static void
bootscreen_progress(int phase, uint64_t done, uint64_t total)
{
	uint32_t permille;

	if (bs_mode == 0 || total == 0) {
		return;
	}
	permille = phase == 0 ? (uint32_t)(done * 850 / total) : 850 + (uint32_t)(done * 150 / total);
	if (phase == bs_phase && bs_shown != 0xffffffff && permille < bs_shown + 5 && done < total) {
		return;
	}
	bs_bar(permille);
	if (phase != bs_phase || phase == 0) {
		bs_status(phase == 0 ? BS_LOADING : BS_CHECKING, phase == 0 ? done : 0, phase == 0 ? total : 0);
	}
	bs_log();
	bs_shown = permille;
	bs_phase = phase;
}

/* the end: the kernel is started ("Starting HakiOS", the bar full), or the image could not be read */
static void
bootscreen_end(int ok)
{
	if (bs_mode == 0) {
		return;
	}
	bs_bar(ok ? 1000 : bs_shown == 0xffffffff ? 0 : bs_shown);
	bs_status(ok ? BS_STARTING : BS_FAILED, 0, 0);
	bs_log();
}
