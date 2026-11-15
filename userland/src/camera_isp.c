/*
 * The cameras' frames made into pictures by the processor (see camera_isp.h): one pixel per 2x2
 * cell, the nearest cell for each pixel of the picture (previews), or every pixel by bilinear
 * demosaic (stills); per pixel, in this order: the black level off, lens shading (a grid of gains per
 * colour, when there is one), the white balance's and AE's gains, the colour matrix (identity until
 * one is set), sRGB's curve. Grey-world white balance; no noise reduction yet. The colour matrix and
 * the lens shading can come from the camera's own tuning (isp_load_tuning(), isp_tune()).
 */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "camera_isp.h"

/* (src/isp_bench.c's build: where a picture's time goes, stage by stage) */
#ifdef ISP_PROFILE
#include <mach/mach_time.h>
uint64_t isp_profile[6];
#define PROFILE_BEGIN() uint64_t profile_at = mach_absolute_time()
#define PROFILE_MARK(i) do { uint64_t t = mach_absolute_time(); isp_profile[i] += t - profile_at; profile_at = t; } while (0)
#else
#define PROFILE_BEGIN() do { } while (0)
#define PROFILE_MARK(i) do { } while (0)
#endif

#define WHITE           959             /* 1023 less the black level of 64 */
#define LUT_WHITE       3836            /* WHITE in the gamma table's steps */

void
isp_init(struct isp *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
	isp->digital = 1;
	isp->zoom = 1;
	for (int i = 0; i < 3; i++) {
		isp->ccm[i][i] = 1;
	}
	for (int i = 0; i < 4096; i++) {
		double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;

		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		isp->gamma[i] = (uint8_t)(v * 255 + 0.5);
	}
}

/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - 64;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - 64;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - 64;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - 64;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

float
isp_sharpness(const struct hakios_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
	enum { BLOCK = 4, MAX_BLOCKS = 512 };
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	uint32_t cx0 = (uint32_t)(x0 * cells_w), cx1 = (uint32_t)(x1 * cells_w), cy0 = (uint32_t)(y0 * cells_h),
	    cy1 = (uint32_t)(y1 * cells_h);
	float above[MAX_BLOCKS], here[MAX_BLOCKS];
	double steps = 0, sum = 0;
	uint32_t blocks_w, rows = 0, n = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	cx1 = cx1 > cells_w ? cells_w : cx1;
	cy1 = cy1 > cells_h ? cells_h : cy1;
	blocks_w = cx1 > cx0 ? (cx1 - cx0) / BLOCK : 0;
	blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
	/*
	 * Green's mean over blocks of 4x4 cells, and the squares of the steps between neighbouring
	 * blocks: a cell's own noise is a quarter of itself in a block's mean, and the picture's edges are
	 * not -- in the dark, at 32x and more, the steps from cell to cell were the noise's alone and the
	 * focus went wherever it liked.
	 */
	/*
	 * (a whole array's frame, the main camera's 2328x1748 cells: blocks of 8x8 cells, every other cell of them taken --
	 * the same part of the picture a block, and a quarter of the cells to unpack: it ran every frame, on one core)
	 */
	uint32_t every = cells_w > 1600 ? 2 : 1, span = BLOCK * every;

	blocks_w = cx1 > cx0 ? (cx1 - cx0) / span : 0;
	blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
	for (uint32_t by = cy0; by + span <= cy1; by += span, rows++) {
		for (uint32_t bx = 0; bx < blocks_w; bx++) {
			uint32_t g = 0;

			for (uint32_t cy = by; cy < by + span; cy += every) {
				const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

				for (uint32_t cx = cx0 + bx * span; cx < cx0 + bx * span + span; cx += every) {
					int v[4];

					cell(top, f->stride, cx, v);
					for (int i = 0; i < 4; i++) {
						g += i != r && i != b ? (uint32_t)v[i] : 0;
					}
				}
			}
			here[bx] = g / (2.0f * BLOCK * BLOCK);
			sum += here[bx];
			n++;
			if (bx > 0) {
				steps += (here[bx] - here[bx - 1]) * (here[bx] - here[bx - 1]);
			}
			if (rows > 0) {
				steps += (here[bx] - above[bx]) * (here[bx] - above[bx]);
			}
		}
		memcpy(above, here, sizeof(float) * blocks_w);
	}
	double mean = n ? sum / n : 0;

	if (level != NULL) {
		*level = (float)mean;
	}
	return mean > 0 ? (float)(steps / n / (mean * mean)) : 0;
}

static inline void lsc_at(const struct isp *isp, float u, float v, float g[3]);

void
isp_measure(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	double sum[3] = { 0, 0, 0 }, grey[2] = { 0, 0 }, neutral[2] = { 0, 0 };
	float gains[3] = { 1, 1, 1 };
	int r, b;

	/*
	 * (every 4th cell of every 4th line: some 64000 of a binned frame's; of a whole array's -- the main camera's 2328x1748
	 * cells -- every 8th, as many, or the frame's measuring alone took longer than a frame on one of the 6T's cores)
	 */
	uint32_t every = cells_w > 1600 ? 8 : 4;

	memset(s, 0, sizeof(*s));
	cell_places(f->bayer, &r, &b);
	for (uint32_t cy = 0; cy < cells_h; cy += every) {
		const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
		float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0;

		for (uint32_t cx = 0; cx < cells_w; cx += every) {
			int v[4], g = 0, top_v = 0;

			cell(top, f->stride, cx, v);
			for (int i = 0; i < 4; i++) {
				g += i != r && i != b ? v[i] : 0;
				top_v = v[i] > top_v ? v[i] : top_v;
			}
			g /= 2;
			sum[0] += v[r];
			sum[1] += g;
			sum[2] += v[b];
			/*
			 * The white balance leaves out a cell with a colour near white -- a lamp, a screen:
			 * green, the most sensitive, stops there first and the cell would count as magenta --
			 * and one near black, where noise cut off at the black level is all there is.
			 */
			if (top_v < WHITE * 9 / 10 && g >= 24) {
				float cr = (float)v[r], cg = (float)g, cb = (float)v[b];

				/*
				 * As the picture will have it: the lens shading raises the corners several times, and
				 * the light there counts for that much more than in the frame as it comes.
				 */
				if (isp != NULL && isp->lsc.on) {
					lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
					cr *= gains[0];
					cg *= gains[1];
					cb *= gains[2];
				}
				/*
				 * Each cell's colour, not its light: summed, a lamp or a screen in the picture -- tens of
				 * times brighter than the room it is in -- would have the white balance to itself, and
				 * the room would be left the colour of its light.
				 */
				grey[0] += cr / cg;
				grey[1] += cb / cg;
				s->grey_cells++;
				/*
				 * Grey, or within a third of it in red and blue, with the white balance as it is: what
				 * the white balance should make grey, without the coloured things around it (the
				 * colour matrix makes any mistake in it twice as plain).
				 */
				if (isp != NULL) {
					float wr = cr * isp->wb[0] / cg, wb = cb * isp->wb[2] / cg;

					if (wr > 0.75f && wr < 1.33f && wb > 0.75f && wb < 1.33f) {
						neutral[0] += cr / cg;
						neutral[1] += cb / cg;
						s->neutral_cells++;
					}
				}
			}
			s->hist[g >> 4 > 63 ? 63 : g >> 4]++;
			s->cells++;
		}
	}
	for (int c = 0; c < 3; c++) {
		s->mean[c] = s->cells ? sum[c] / s->cells : 0;
	}
	for (int c = 0; c < 2; c++) {
		s->grey[c] = s->grey_cells ? grey[c] / s->grey_cells : 0;
		s->neutral[c] = s->neutral_cells ? neutral[c] / s->neutral_cells : 0;
	}
	for (int i = 63, above = 0; i >= 0; i--) {
		above += s->hist[i];
		if (above * 100 >= (int)s->cells) {
			s->bright = i * 16 + 8;
			break;
		}
	}
}

/*
 * Green's mean towards 15 % of white, unless that would put 1 % of the picture into white. The
 * exposure, longest first: the frame's lines at the shortest frame (min_frame_lines', when that is longer than the
 * mode's), then gain to 16x, then frames up to max_frame_lines, then gain to the most; beyond that, digital gain (up to 4x).
 */
bool
isp_ae(struct isp *isp, const struct hakios_camera_frame *f, const struct isp_stats *s, struct hakios_camera_command *c)
{
	double now = (double)f->exposure_lines * f->gain / f->gain_unity * isp->digital, ratio, want;
	double target = 0.15 * WHITE, mean = s->mean[1] * isp->digital > 0.5 ? s->mean[1] * isp->digital : 0.5;
	double bright = s->bright * isp->digital;
	uint32_t shortest = isp->min_frame_lines > f->frame_lines_min ? isp->min_frame_lines : f->frame_lines_min;
	uint32_t fast = shortest - f->margin, longest = isp->max_frame_lines ? isp->max_frame_lines : 2 * shortest;
	uint32_t lines, gain, frame_lines = shortest > f->frame_lines_min ? shortest : 0;
	double digital = 1;

	if (isp->asked && (f->exposure_lines != isp->asked_lines || f->gain != isp->asked_gain)) {
		return false;                   /* (the last change not in the frames yet) */
	}
	/* (the measured levels are the sensor's: digital gain is applied to the picture only) */
	ratio = target / mean;
	if (bright * ratio > 0.9 * WHITE && bright > 0) {
		ratio = 0.9 * WHITE / bright > target / mean * 0.5 ? 0.9 * WHITE / bright : target / mean * 0.5;
	}
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	if (ratio > 0.94 && ratio < 1.06) {
		return false;
	}
	want = now * pow(ratio, 0.7);
	if (want <= fast) {
		lines = want < 4 ? 4 : (uint32_t)want;
		gain = f->gain_unity;
	} else if (want <= (double)fast * 16) {
		lines = fast;
		gain = (uint32_t)(want / fast * f->gain_unity);
	} else {
		longest = longest < shortest ? shortest : longest;
		longest = f->frame_lines_max != 0 && longest > f->frame_lines_max ? f->frame_lines_max : longest;
		lines = (uint32_t)(want / 16);
		lines = lines > longest - f->margin ? longest - f->margin : lines < fast ? fast : lines;
		frame_lines = lines + f->margin;
		double g = want / lines;

		if (g > (double)f->gain_max / f->gain_unity) {
			digital = g / ((double)f->gain_max / f->gain_unity);
			digital = digital > 4 ? 4 : digital;
			g = (double)f->gain_max / f->gain_unity;
		}
		gain = (uint32_t)(g * f->gain_unity);
	}
	isp->digital = (float)digital;
	isp->asked_lines = lines;
	isp->asked_gain = gain;
	isp->asked_frame_lines = frame_lines;
	isp->asked = true;
	memset(c, 0, sizeof(*c));
	c->command = HAKIOS_CAMERA_EXPOSURE;
	c->a = lines;
	c->b = gain;
	c->c = frame_lines;
	return true;
}

/*
 * The point of the reference lights' line nearest a light seen as red over green rg and blue over
 * green bg, on their logarithms: where along it (segment i and how far into it, 0 to 1).
 */
static float
on_reference_line(const struct isp_tuning *t, float rg, float bg, uint32_t *segment, float *along)
{
	float x = logf(rg), y = logf(bg), best = INFINITY;

	*segment = 0;
	*along = 0;
	for (uint32_t i = 0; i + 1 < t->refs; i++) {
		float x0 = logf(t->ref[i][0]), y0 = logf(t->ref[i][1]), dx = logf(t->ref[i + 1][0]) - x0,
		    dy = logf(t->ref[i + 1][1]) - y0, length = dx * dx + dy * dy;
		float u = length > 0 ? ((x - x0) * dx + (y - y0) * dy) / length : 0, d;

		u = u < 0 ? 0 : u > 1 ? 1 : u;
		d = (x0 + u * dx - x) * (x0 + u * dx - x) + (y0 + u * dy - y) * (y0 + u * dy - y);
		if (d < best) {
			best = d;
			*segment = i;
			*along = u;
		}
	}
	return sqrtf(best);
}

float
isp_cct(const struct isp *isp)
{
	const struct isp_tuning *t = isp->tuning;
	uint32_t i;
	float u;

	if (t == NULL || t->refs < 2 || isp->wb[0] <= 0 || isp->wb[2] <= 0) {
		return 0;
	}
	on_reference_line(t, 1 / isp->wb[0], 1 / isp->wb[2], &i, &u);
	return t->ref[i][2] + u * (t->ref[i + 1][2] - t->ref[i][2]);
}

void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
	/*
	 * The nearly grey cells, when there are enough of them (1 in 32); else every cell neither near
	 * white nor near black, when there are enough of those (1 in 16); else every cell's light.
	 */
	float r, b;

	if (s->neutral_cells * 32 >= s->cells && s->neutral[0] > 0.01 && s->neutral[1] > 0.01) {
		r = (float)(1 / s->neutral[0]);
		b = (float)(1 / s->neutral[1]);
	} else if (s->grey_cells * 16 >= s->cells && s->grey[0] > 0.01 && s->grey[1] > 0.01) {
		r = (float)(1 / s->grey[0]);
		b = (float)(1 / s->grey[1]);
	} else if (s->mean[0] >= 2 && s->mean[1] >= 2 && s->mean[2] >= 2) {
		r = (float)(s->mean[1] / s->mean[0]);
		b = (float)(s->mean[1] / s->mean[2]);
	} else {
		return;
	}

	/*
	 * Onto the line of the reference lights: a scene's own colours pull grey world off it -- a pink
	 * screen, a green wall -- and off it is what shows as green or magenta, while along it is a
	 * warmer or colder white. (The wide camera under a warm white LED: grey world of the room with a
	 * screen in it 15 % short in both red and blue, the light measured on a white sheet; kept to the
	 * line, 2 % off.)
	 */
	if (isp->tuning != NULL && isp->tuning->refs >= 2) {
		const struct isp_tuning *t = isp->tuning;
		uint32_t i;
		float u, d = on_reference_line(t, 1 / r, 1 / b, &i, &u);
		float lx = logf(t->ref[i][0]) + u * (logf(t->ref[i + 1][0]) - logf(t->ref[i][0]));
		float ly = logf(t->ref[i][1]) + u * (logf(t->ref[i + 1][1]) - logf(t->ref[i][1]));

		/*
		 * Within awb_band of it (in logarithms: 0.1 is 10 %), left where grey world puts it: lights are not
		 * all on the line -- the OnePlus 6T's references bend from daylight through its fluorescent tubes
		 * to a filament lamp, and a warm white LED there falls below that bend, which the line itself made
		 * yellow-green (its front camera's: 0.15 kept the ceiling a warm white). Further off, to the band's edge.
		 */
		if (d > isp->awb_band) {
			float k = isp->awb_band / d;

			r = 1 / expf(lx + (-logf(r) - lx) * k);
			b = 1 / expf(ly + (-logf(b) - ly) * k);
		}
	}
	r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
	b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
	isp->wb[0] = isp->wb[0] * 0.9f + r * 0.1f;
	isp->wb[2] = isp->wb[2] * 0.9f + b * 0.1f;
}

/* the part of the sensor's cells the picture shows: its middle, in the picture's proportions, 1/zoom of it each way */
static void
crop(const struct isp *isp, const struct hakios_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0,
    uint32_t *cw, uint32_t *ch)
{
	uint32_t w = f->width / 2, h = f->height / 2;
	float zoom = isp->zoom < 1 ? 1 : isp->zoom;

	if ((uint64_t)width * h > (uint64_t)height * w) {
		*cw = w;
		*ch = (uint32_t)((uint64_t)w * height / width);
	} else {
		*ch = h;
		*cw = (uint32_t)((uint64_t)h * width / height);
	}
	*cw = (uint32_t)(*cw / zoom) < 2 ? 2 : (uint32_t)(*cw / zoom);
	*ch = (uint32_t)(*ch / zoom) < 2 ? 2 : (uint32_t)(*ch / zoom);
	*x0 = (w - *cw) / 2;
	*y0 = (h - *ch) / 2;
}

void
isp_set_ccm(struct isp *isp, const float m[9])
{
	for (int i = 0; i < 9; i++) {
		isp->ccm[i / 3][i % 3] = m[i];
	}
}

/*
 * The colour matrix with the white balance's and AE's gains folded in, in the gamma table's steps
 * (per 10-bit step above black): out[c] = sum over j of m[c][j] * value[j] (each value lens-shaded).
 */
struct pipe {
	float           m[3][3];
	bool            identity;
};

static void
pipe_init(const struct isp *isp, struct pipe *p)
{
	p->identity = true;
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			p->m[c][j] = isp->ccm[c][j] * isp->wb[j] * isp->digital * (LUT_WHITE / (float)WHITE);
			p->identity = p->identity && (c == j ? isp->ccm[c][j] == 1 : isp->ccm[c][j] == 0);
		}
	}
}

static inline uint8_t
curve(const struct isp *isp, float v)
{
	int i = (int)(v + 0.5f);

	return isp->gamma[i < 0 ? 0 : i > 4095 ? 4095 : i];
}

static inline void
pixel(const struct isp *isp, const struct pipe *p, float r, float g, float b, uint8_t *rgb)
{
	if (p->identity) {
		rgb[0] = curve(isp, r * p->m[0][0]);
		rgb[1] = curve(isp, g * p->m[1][1]);
		rgb[2] = curve(isp, b * p->m[2][2]);
		return;
	}
	rgb[0] = curve(isp, p->m[0][0] * r + p->m[0][1] * g + p->m[0][2] * b);
	rgb[1] = curve(isp, p->m[1][0] * r + p->m[1][1] * g + p->m[1][2] * b);
	rgb[2] = curve(isp, p->m[2][0] * r + p->m[2][1] * g + p->m[2][2] * b);
}

/* the lens shading's gains at (u, v), 0 to 1 across the sensor */
static inline void
lsc_at(const struct isp *isp, float u, float v, float g[3])
{
	float gx = u * (LSC_W - 1), gy = v * (LSC_H - 1);
	int x = (int)gx, y = (int)gy;

	x = x < 0 ? 0 : x > LSC_W - 2 ? LSC_W - 2 : x;
	y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
	float fx = gx - x, fy = gy - y;

	for (int c = 0; c < 3; c++) {
		const float (*t)[LSC_W] = isp->lsc.gain[c];

		g[c] = (t[y][x] * (1 - fx) + t[y][x + 1] * fx) * (1 - fy) + (t[y + 1][x] * (1 - fx) + t[y + 1][x + 1] * fx) * fy;
	}
}

/*
 * What every line of a picture needs, worked out once for all of a piece of it, so that the work per pixel is a few
 * integer operations on whole lines the compiler does many at a time (NEON) -- per pixel, in floating point and scalar
 * code, the preview took 50-70 ms a frame on one of the OnePlus 6T's cores (isp_bench, r193; this Mac 8), 200 ms of
 * processor time a frame in mediaserverd, 10-16 frames a second:
 * - which cells a line of the picture is made of: magnified (the preview's 1504x1128 or 1920x1080 of 1164x873 or
 *   1152x864 cells, and the zoom), each pixel between two cells across and two lines of them; else (the main camera's
 *   whole array, 2328x1748 cells) the nearest cell;
 * - a line of cells, each unpacked, lens-shaded (the grid interpolated down to the line first), through the colour
 *   matrix with the gains and the gamma -- once a cell, not once a pixel -- then across to the picture's pixels, two such
 *   lines kept (lines of the picture go between the same two);
 * - per line of the picture, between the two (in the gamma's 8-bit steps, as a scaler does), and to NV12.
 */
struct render {
	const struct isp *isp;
	const struct hakios_camera_frame *f;
	const uint8_t *raw;
	uint32_t        width, height, x0, y0, cw, ch, cells_w, cells_h;
	int             r, b;
	bool            magnified;
	struct pipe     p;
	uint32_t        first, count;   /* the cells a line needs, from `first' */
	uint32_t        *k0, *k1;       /* each pixel's cell (and magnified the next one across), from `first' ... */
	uint32_t        *wx;            /* ... and the second's weight, 0 to 256 */
	uint32_t        *lsc_x;         /* each cell's column of the lens shading's grid (the left of two) ... */
	float           *lsc_fx;        /* ... and how far towards the next */
	float           *cell[3];       /* a line of cells, R, G (the mean of the two), B: linear, lens-shaded ... */
	int32_t         *at[3];         /* ... through the colour matrix, in the gamma table's steps ... */
	uint8_t         *c8[3];         /* ... and through the gamma */
	uint16_t        *line[2][3];    /* two lines of cells across to the picture's pixels, R, G, B (8 bits times 256) */
	uint32_t        cached[2];      /* which lines of cells they are (~0u: none) */
	void            *memory;
};

static void
render_end(struct render *rd)
{
	free(rd->memory);
	memset(rd, 0, sizeof(*rd));
}

static bool
render_begin(struct render *rd, const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint32_t width,
    uint32_t height)
{
	uint32_t lo = ~0u, hi = 0;

	memset(rd, 0, sizeof(*rd));
	rd->isp = isp;
	rd->f = f;
	rd->raw = raw;
	rd->width = width;
	rd->height = height;
	rd->cells_w = f->width / 2;
	rd->cells_h = f->height / 2;
	crop(isp, f, width, height, &rd->x0, &rd->y0, &rd->cw, &rd->ch);
	cell_places(f->bayer, &rd->r, &rd->b);
	pipe_init(isp, &rd->p);
	rd->magnified = rd->cw < width && rd->ch < height;

	/* (one allocation for all of it: a line's cells are at most cells_w, or width where each pixel is its own cell) */
	size_t n = width, cells = rd->cells_w > n ? rd->cells_w : n;
	size_t bytes = (3 * n + cells) * 4 /* k0 k1 wx lsc_x */ + cells * 4 /* lsc_fx */ + 3 * cells * 4 /* cell */ +
	    3 * cells * 4 /* at */ + 3 * cells /* c8 */ + 2 * 3 * n * 2 /* line */ + 64;

	rd->memory = malloc(bytes);
	if (rd->memory == NULL) {
		return false;
	}
	uint8_t *m = rd->memory;
#define TAKE(p, type, count) do { (p) = (type *)(void *)m; m += ((count) * sizeof(type) + 15) & ~(size_t)15; } while (0)
	TAKE(rd->k0, uint32_t, n);
	TAKE(rd->k1, uint32_t, n);
	TAKE(rd->wx, uint32_t, n);
	TAKE(rd->lsc_x, uint32_t, cells);
	TAKE(rd->lsc_fx, float, cells);
	for (int c = 0; c < 3; c++) {
		TAKE(rd->cell[c], float, cells);
		TAKE(rd->at[c], int32_t, cells);
		TAKE(rd->c8[c], uint8_t, cells);
	}
	for (int l = 0; l < 2; l++) {
		for (int c = 0; c < 3; c++) {
			TAKE(rd->line[l][c], uint16_t, n);
		}
	}
#undef TAKE
	if ((size_t)(m - (uint8_t *)rd->memory) > bytes + 16 * 16) {
		render_end(rd);
		return false;
	}

	uint64_t step = ((uint64_t)rd->cw << 16) / width, pos = (uint64_t)rd->x0 << 16;

	for (uint32_t x = 0; x < width; x++, pos += step) {
		uint32_t cx0, cx1;

		if (rd->magnified) {
			float fx = rd->x0 + (x + 0.5f) * rd->cw / width - 0.5f, tx;

			cx0 = fx < 0 ? 0 : (uint32_t)fx;
			cx0 = cx0 > rd->cells_w - 1 ? rd->cells_w - 1 : cx0;
			cx1 = cx0 + 1 > rd->cells_w - 1 ? rd->cells_w - 1 : cx0 + 1;
			tx = fx < (float)cx0 ? 0 : fx - cx0;
			rd->wx[x] = (uint32_t)(tx * 256 + 0.5f);
		} else {
			cx0 = cx1 = (uint32_t)(pos >> 16);
			rd->wx[x] = 0;
		}
		rd->k0[x] = cx0;
		rd->k1[x] = cx1;
		lo = cx0 < lo ? cx0 : lo;
		hi = cx1 > hi ? cx1 : hi;
	}
	rd->first = lo;
	rd->count = hi - lo + 1;
	for (uint32_t x = 0; x < width; x++) {
		rd->k0[x] -= lo;
		rd->k1[x] -= lo;
	}
	/* (the lens shading's columns: per cell of the range, magnified; per pixel's cell, else) */
	for (uint32_t k = 0; k < (rd->magnified ? rd->count : width); k++) {
		uint32_t cx = lo + (rd->magnified ? k : rd->k0[k]);
		float gx = (rd->cells_w > 1 ? (float)cx / (rd->cells_w - 1) : 0) * (LSC_W - 1);
		uint32_t gx0 = gx < 0 ? 0 : (uint32_t)gx;

		gx0 = gx0 > LSC_W - 2 ? LSC_W - 2 : gx0;
		rd->lsc_x[k] = gx0;
		rd->lsc_fx[k] = gx - gx0;
	}
	rd->cached[0] = rd->cached[1] = ~0u;
	return true;
}

/*
 * The cells first..first+n of the pair of lines from `top' as R, G (the mean of the two), B above black: two cells to a
 * five-byte group of RAW10 in each line, the group read as one 64-bit word -- cell() for each of them was the slowest
 * part of a picture on the 6T (20 of 47 ms on one core, isp_bench r197). The groups whose eight bytes would go past the
 * line's (the last ones of the frame) and a first or last cell alone in its group go through cell().
 */
static void
unpack_cells(const struct render *rd, const uint8_t *top, uint32_t first, uint32_t n, const uint32_t *pick, float *cr, float *cg,
    float *cb)
{
	size_t stride = rd->f->stride;
	int r = rd->r, b = rd->b;
	uint32_t k = 0;

	if (pick != NULL) {
		/* each pixel's own cell (pick[0..n), from `first'): its group's two 64-bit words, the half it is in */
		uint32_t last = (uint32_t)((stride - 8) / 5);

		for (; k < n; k++) {
			uint32_t cx = first + pick[k], group = cx >> 1, half = cx & 1;
			int v[4];

			if (group <= last) {
				const uint8_t *g0 = top + (size_t)group * 5;
				uint64_t a, c;

				memcpy(&a, g0, 8);
				memcpy(&c, g0 + stride, 8);

				uint32_t sh = half * 16, lsh = 32 + half * 4;

				v[0] = (int)(((a >> sh & 0xff) << 2) | (a >> lsh & 3)) - 64;
				v[1] = (int)(((a >> (sh + 8) & 0xff) << 2) | (a >> (lsh + 2) & 3)) - 64;
				v[2] = (int)(((c >> sh & 0xff) << 2) | (c >> lsh & 3)) - 64;
				v[3] = (int)(((c >> (sh + 8) & 0xff) << 2) | (c >> (lsh + 2) & 3)) - 64;
				for (int j = 0; j < 4; j++) {
					v[j] = v[j] < 0 ? 0 : v[j];
				}
			} else {
				cell(top, stride, cx, v);
			}
			cr[k] = (float)v[r];
			cg[k] = (v[0] + v[1] + v[2] + v[3] - v[r] - v[b]) * 0.5f;
			cb[k] = (float)v[b];
		}
		return;
	}
	if ((first & 1) && k < n) {
		int v[4];

		cell(top, stride, first, v);
		cr[0] = (float)v[r];
		cg[0] = (v[0] + v[1] + v[2] + v[3] - v[r] - v[b]) * 0.5f;
		cb[0] = (float)v[b];
		k = 1;
	}
	/* (whole groups while a group's eight bytes stay within the line's stride) */
	uint32_t group = (first + k) >> 1, last_group = (uint32_t)((stride - 8) / 5);

	for (; k + 1 < n && group <= last_group; k += 2, group++) {
		const uint8_t *g0 = top + (size_t)group * 5, *g1 = g0 + stride;
		uint64_t a, c;

		memcpy(&a, g0, 8);
		memcpy(&c, g1, 8);

		int v0[4], v1[4];
		uint32_t la = (uint32_t)(a >> 32), lc = (uint32_t)(c >> 32);

		/* cell 0: pixels 0 and 1 of each line; cell 1: pixels 2 and 3 (positions 0 top left .. 3 bottom right) */
		v0[0] = (int)(((a & 0xff) << 2) | (la & 3)) - 64;
		v0[1] = (int)(((a >> 8 & 0xff) << 2) | (la >> 2 & 3)) - 64;
		v0[2] = (int)(((c & 0xff) << 2) | (lc & 3)) - 64;
		v0[3] = (int)(((c >> 8 & 0xff) << 2) | (lc >> 2 & 3)) - 64;
		v1[0] = (int)(((a >> 16 & 0xff) << 2) | (la >> 4 & 3)) - 64;
		v1[1] = (int)(((a >> 24 & 0xff) << 2) | (la >> 6 & 3)) - 64;
		v1[2] = (int)(((c >> 16 & 0xff) << 2) | (lc >> 4 & 3)) - 64;
		v1[3] = (int)(((c >> 24 & 0xff) << 2) | (lc >> 6 & 3)) - 64;
		for (int j = 0; j < 4; j++) {
			v0[j] = v0[j] < 0 ? 0 : v0[j];
			v1[j] = v1[j] < 0 ? 0 : v1[j];
		}
		cr[k] = (float)v0[r];
		cg[k] = (v0[0] + v0[1] + v0[2] + v0[3] - v0[r] - v0[b]) * 0.5f;
		cb[k] = (float)v0[b];
		cr[k + 1] = (float)v1[r];
		cg[k + 1] = (v1[0] + v1[1] + v1[2] + v1[3] - v1[r] - v1[b]) * 0.5f;
		cb[k + 1] = (float)v1[b];
	}
	for (; k < n; k++) {
		int v[4];

		cell(top, stride, first + k, v);
		cr[k] = (float)v[r];
		cg[k] = (v[0] + v[1] + v[2] + v[3] - v[r] - v[b]) * 0.5f;
		cb[k] = (float)v[b];
	}
}

/*
 * The cells of a line through the lens shading, the colour matrix and the gamma: cells[c][0..n) in (linear, above
 * black), c8[c][0..n) out; the lens shading's columns lsc_x/lsc_fx[0..n).
 */
static void
cells_to_8bit(struct render *rd, uint32_t cy, uint32_t n)
{
	const struct isp *isp = rd->isp;
	float *cr = rd->cell[0], *cg = rd->cell[1], *cb = rd->cell[2];

	if (isp->lsc.on) {
		/* the grid down to this line first (as lsc_at(): its corners on the sensor's), then across, per cell */
		float gy = (rd->cells_h > 1 ? (float)cy / (rd->cells_h - 1) : 0) * (LSC_H - 1), row[3][LSC_W];
		int y = (int)gy;

		y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
		float fy = gy - y;

		for (int c = 0; c < 3; c++) {
			for (int x = 0; x < LSC_W; x++) {
				row[c][x] = isp->lsc.gain[c][y][x] * (1 - fy) + isp->lsc.gain[c][y + 1][x] * fy;
			}
		}
		for (uint32_t k = 0; k < n; k++) {
			uint32_t x = rd->lsc_x[k];
			float fx = rd->lsc_fx[k];

			cr[k] *= row[0][x] * (1 - fx) + row[0][x + 1] * fx;
			cg[k] *= row[1][x] * (1 - fx) + row[1][x + 1] * fx;
			cb[k] *= row[2][x] * (1 - fx) + row[2][x + 1] * fx;
		}
	}
	const float m00 = rd->p.m[0][0], m01 = rd->p.m[0][1], m02 = rd->p.m[0][2], m10 = rd->p.m[1][0], m11 = rd->p.m[1][1],
	    m12 = rd->p.m[1][2], m20 = rd->p.m[2][0], m21 = rd->p.m[2][1], m22 = rd->p.m[2][2];
	const uint8_t *gamma = isp->gamma;
	uint8_t *o0 = rd->c8[0], *o1 = rd->c8[1], *o2 = rd->c8[2];

	/* (the matrix and the table in one pass: no line of indexes written and read back) */
	for (uint32_t k = 0; k < n; k++) {
		float r = cr[k], g = cg[k], b = cb[k];
		int32_t i0 = (int32_t)(m00 * r + m01 * g + m02 * b + 0.5f);
		int32_t i1 = (int32_t)(m10 * r + m11 * g + m12 * b + 0.5f);
		int32_t i2 = (int32_t)(m20 * r + m21 * g + m22 * b + 0.5f);

		o0[k] = gamma[i0 < 0 ? 0 : i0 > 4095 ? 4095 : i0];
		o1[k] = gamma[i1 < 0 ? 0 : i1 > 4095 ? 4095 : i1];
		o2[k] = gamma[i2 < 0 ? 0 : i2 > 4095 ? 4095 : i2];
	}
}

/* the line of cells cy across to the picture's pixels: kept, or made in place of the kept line that is not `keep' */
static uint16_t *const *
cell_line(struct render *rd, uint32_t cy, uint32_t keep)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (rd->cached[i] == cy) {
			return rd->line[i];
		}
	}
	i = rd->cached[0] == keep ? 1 : 0;

	const uint8_t *top = rd->raw + (size_t)cy * 2 * rd->f->stride;
	float *cr = rd->cell[0], *cg = rd->cell[1], *cb = rd->cell[2];
	uint32_t n = rd->magnified ? rd->count : rd->width;
	PROFILE_BEGIN();

	/* magnified: every cell of the range; else each pixel's own (a line of the picture takes 1504 of 2328) */
	unpack_cells(rd, top, rd->first, n, rd->magnified ? NULL : rd->k0, cr, cg, cb);
	PROFILE_MARK(0);
	cells_to_8bit(rd, cy, n);
	PROFILE_MARK(1);
	const uint8_t *i0 = rd->c8[0], *i1 = rd->c8[1], *i2 = rd->c8[2];
	uint16_t *o0 = rd->line[i][0], *o1 = rd->line[i][1], *o2 = rd->line[i][2];

	if (rd->magnified) {
		/* (the three colours in one pass: each pixel's two cells and weight read once) */
		for (uint32_t x = 0; x < rd->width; x++) {
			uint32_t a = rd->k0[x], d = rd->k1[x], w = rd->wx[x], v = 256 - w;

			o0[x] = (uint16_t)(i0[a] * v + i0[d] * w);
			o1[x] = (uint16_t)(i1[a] * v + i1[d] * w);
			o2[x] = (uint16_t)(i2[a] * v + i2[d] * w);
		}
	} else {
		for (uint32_t x = 0; x < rd->width; x++) {
			o0[x] = (uint16_t)(i0[x] << 8);
			o1[x] = (uint16_t)(i1[x] << 8);
			o2[x] = (uint16_t)(i2[x] << 8);
		}
	}
	PROFILE_MARK(2);
	rd->cached[i] = cy;
	return rd->line[i];
}

/* one line of the picture as 8-bit R, G, B, each in a line of its own */
static void
render_line(struct render *rd, uint32_t row, uint8_t *const out[3])
{
	const struct isp *isp = rd->isp;
	uint32_t width = rd->width, height = rd->height, wy = 0;
	uint16_t *const *a, *const *c;

	if (isp->turned) {
		row = height - 1 - row;         /* (and each line from its end, below) */
	}
	if (rd->magnified) {
		/*
		 * Magnified (the zoom, and the preview): each pixel between the four cells around it. The nearest
		 * cell blown up to a square of pixels showed every cell's own colour, its noise and the colours a
		 * 2x2 cell gets wrong at an edge, as coloured dots.
		 */
		float fy = rd->y0 + (row + 0.5f) * rd->ch / height - 0.5f, ty;
		uint32_t cy0 = fy < 0 ? 0 : (uint32_t)fy, cy1;

		cy0 = cy0 > rd->cells_h - 1 ? rd->cells_h - 1 : cy0;
		cy1 = cy0 + 1 > rd->cells_h - 1 ? rd->cells_h - 1 : cy0 + 1;
		ty = fy < (float)cy0 ? 0 : fy - cy0;
		wy = (uint32_t)(ty * 256 + 0.5f);
		a = cell_line(rd, cy0, cy1);
		c = cell_line(rd, cy1, cy0);
	} else {
		uint32_t cy = rd->y0 + (uint32_t)((uint64_t)row * rd->ch / height);

		a = c = cell_line(rd, cy, ~0u);
	}
	PROFILE_BEGIN();
	for (int k = 0; k < 3; k++) {
		const uint16_t *p = a[k], *q = c[k];
		uint8_t *o = out[k];

		if (isp->turned) {
			for (uint32_t x = 0; x < width; x++) {
				o[width - 1 - x] = (uint8_t)((p[x] * (256 - wy) + q[x] * wy + 32768) >> 16);
			}
		} else {
			for (uint32_t x = 0; x < width; x++) {
				o[x] = (uint8_t)((p[x] * (256 - wy) + q[x] * wy + 32768) >> 16);
			}
		}
	}
	PROFILE_MARK(3);
}

/*
 * Two lines of 8-bit R, G, B (each colour a line of its own) as NV12 (BT.601's matrix, which CoreVideo's 420f and 420v
 * and tools/camraw.py take): each pixel's Y (y1 NULL: the second line has none, the picture's last of an odd height),
 * each 2x2's Cb and Cr from the sum of its four. The coefficients in 1/65536ths, video range's scaled to its 219 steps
 * of luma (16 to 235) and 224 of chroma (16 to 240); every value rounded to the nearest step, so that the primaries come
 * back within one step of 0 and 255 (red's and blue's cannot be exact: no 8-bit Y, Cb, Cr is both).
 */
static void
nv12_pair(uint8_t *const l0[3], uint8_t *const l1[3], uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
	static const int32_t full[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
	static const int32_t video[9] = { 16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681 };
	const int32_t *k = video_range ? video : full;
	const int32_t k0 = k[0], k1 = k[1], k2 = k[2], k3 = k[3], k4 = k[4], k5 = k[5], k6 = k[6], k7 = k[7], k8 = k[8];
	int32_t oy = (video_range ? 16 << 16 : 0) + (1 << 15);
	PROFILE_BEGIN();

	for (int l = 0; l < 2; l++) {
		uint8_t *const *p = l ? l1 : l0;
		const uint8_t *r = p[0], *g = p[1], *b = p[2];
		uint8_t *out = l ? y1 : y0;

		if (out == NULL) {
			continue;
		}
		for (uint32_t x = 0; x < width; x++) {
			out[x] = (uint8_t)((k0 * r[x] + k1 * g[x] + k2 * b[x] + oy) >> 16);
		}
	}
	/*
	 * (the sums are four pixels': 18 bits off, with 128 added and just under half a step to round; a tie goes down, so
	 * that chroma's -127.5, a full colour's, comes out 0 as its +127.5 does 255)
	 */
	const uint8_t *r0 = l0[0], *g0 = l0[1], *b0 = l0[2], *r1 = l1[0], *g1 = l1[1], *b1 = l1[2];
	uint32_t pairs = width / 2;

	for (uint32_t i = 0; i < pairs; i++) {
		uint32_t x = 2 * i;
		int32_t R = r0[x] + r0[x + 1] + r1[x] + r1[x + 1];
		int32_t G = g0[x] + g0[x + 1] + g1[x] + g1[x + 1];
		int32_t B = b0[x] + b0[x + 1] + b1[x] + b1[x + 1];
		int32_t cb = (k3 * R + k4 * G + k5 * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k6 * R + k7 * G + k8 * B + (128 << 18) + (1 << 17) - 1) >> 18;

		uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
	if (width & 1) {
		uint32_t x = width - 1;
		int32_t R = 2 * (r0[x] + r1[x]), G = 2 * (g0[x] + g1[x]), B = 2 * (b0[x] + b1[x]);
		int32_t cb = (k3 * R + k4 * G + k5 * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k6 * R + k7 * G + k8 * B + (128 << 18) + (1 << 17) - 1) >> 18;

		uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
	PROFILE_MARK(4);
}

/* (interleaved 8-bit R, G, B lines, for the still path's callers of nv12_pair) */
static void
nv12_pair_rgb(const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
	static __thread uint8_t *planes;
	static __thread uint32_t planned;

	if (width > planned) {
		free(planes);
		planes = malloc(6 * (size_t)width);
		planned = planes != NULL ? width : 0;
	}
	if (planes == NULL) {
		return;
	}
	uint8_t *const l0[3] = { planes, planes + width, planes + 2 * (size_t)width };
	uint8_t *const l1[3] = { planes + 3 * (size_t)width, planes + 4 * (size_t)width, planes + 5 * (size_t)width };

	for (uint32_t x = 0; x < width; x++) {
		l0[0][x] = rgb0[3 * x];
		l0[1][x] = rgb0[3 * x + 1];
		l0[2][x] = rgb0[3 * x + 2];
		l1[0][x] = rgb1[3 * x];
		l1[1][x] = rgb1[3 * x + 1];
		l1[2][x] = rgb1[3 * x + 2];
	}
	nv12_pair(l0, l1, width, y0, y1, uv, video_range);
}

void
isp_render_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	struct render rd;
	uint8_t *planes;

	if (width > 8192 || !render_begin(&rd, isp, f, raw, width, height) || (planes = malloc(6 * (size_t)width)) == NULL) {
		if (rd.memory != NULL) {
			render_end(&rd);
		}
		return;
	}
	uint8_t *const l0[3] = { planes, planes + width, planes + 2 * (size_t)width };
	uint8_t *const l1[3] = { planes + 3 * (size_t)width, planes + 4 * (size_t)width, planes + 5 * (size_t)width };

	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		bool second = row + 1 < height;

		render_line(&rd, row, l0);
		render_line(&rd, second ? row + 1 : row, l1);
		nv12_pair(l0, l1, width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
	free(planes);
	render_end(&rd);
}

void
isp_render_bgra(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[3 * 8192], o[4 * 8192];
	uint8_t *const planes[3] = { line, line + 8192, line + 2 * 8192 };
	struct render rd;

	if (width > 8192 || !render_begin(&rd, isp, f, raw, width, height)) {
		return;
	}
	for (uint32_t row = first_row; row < end_row && row < height; row++) {
		render_line(&rd, row, planes);
		for (uint32_t x = 0; x < width; x++) {
			o[4 * x] = planes[2][x];
			o[4 * x + 1] = planes[1][x];
			o[4 * x + 2] = planes[0][x];
			o[4 * x + 3] = 255;
		}
		memcpy(out + (size_t)row * stride, o, 4 * (size_t)width);      /* (a line at a time: few, wide writes) */
	}
	render_end(&rd);
}

/* ---- every pixel: bilinear demosaic -------------------------------------------------------------------- */

/* the colour of each site of a 2x2 cell (0 R, 1 G, 2 B), position y * 2 + x */
static void
cfa(uint32_t bayer, int colour[4])
{
	int r, b;

	cell_places(bayer, &r, &b);
	for (int i = 0; i < 4; i++) {
		colour[i] = i == r ? 0 : i == b ? 2 : 1;
	}
}

/* line y of the frame as values above black (y clamped into the frame) */
static void
unpack_line(const struct hakios_camera_frame *f, const uint8_t *raw, int y, int *out)
{
	y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
	const uint8_t *line = raw + (size_t)y * f->stride;

	for (uint32_t x = 0; x + 3 < f->width + 3 && x < f->width; x += 4) {
		const uint8_t *g = line + (x >> 2) * 5;

		for (int i = 0; i < 4 && x + i < f->width; i++) {
			int v = (g[i] << 2 | (g[4] >> (i * 2) & 3)) - (int)f->black;

			out[x + i] = v < 0 ? 0 : v;
		}
	}
}

/* one line (y) at full size as 8-bit R, G, B, from the lines y - 1, y, y + 1 (l[0..2]) */
static void
full_line(const struct isp *isp, const struct hakios_camera_frame *f, const int *const l[3], uint32_t y, const int colour[4],
    const struct pipe *p, uint8_t *rgb)
{
	int w = (int)f->width;
	float v_pos = f->height > 1 ? (float)y / (f->height - 1) : 0, gains[3] = { 1, 1, 1 };

	for (int x = 0; x < w; x++) {
		float sum[3] = { 0, 0, 0 };
		int n[3] = { 0, 0, 0 };

		/* each colour: its own value where the site has it, else the mean of its sites in the 3x3 around */
		for (int dy = -1; dy <= 1; dy++) {
			const int *line = l[dy + 1];
			int yy = (int)y + dy;

			yy = yy < 0 ? 0 : yy >= (int)f->height ? (int)f->height - 1 : yy;
			for (int dx = -1; dx <= 1; dx++) {
				int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
				int c = colour[(yy & 1) * 2 + (xx & 1)];

				if (dx == 0 && dy == 0) {
					sum[c] = (float)line[xx] * 64;          /* (its own value wins over the neighbours') */
					n[c] = 64;
				} else if (n[c] < 64) {
					sum[c] += (float)line[xx];
					n[c]++;
				}
			}
		}
		if (isp->lsc.on) {
			lsc_at(isp, w > 1 ? (float)x / (w - 1) : 0, v_pos, gains);
		}
		pixel(isp, p, sum[0] / (n[0] ? n[0] : 1) * gains[0], sum[1] / (n[1] ? n[1] : 1) * gains[1],
		    sum[2] / (n[2] ? n[2] : 1) * gains[2], rgb + 3 * x);
	}
}

void
isp_render_full_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t width = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[4];
	static __thread uint8_t *rgb[2];
	static __thread uint32_t have;

	if (width > have) {
		for (int i = 0; i < 4; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * width);
		}
		for (int i = 0; i < 2; i++) {
			free(rgb[i]);
			rgb[i] = malloc(3 * (size_t)width);
		}
		have = width;
	}
	cfa(f->bayer, colour);
	pipe_init(isp, &p);
	for (uint32_t row = first_row & ~1u; row < end_row && row < f->height; row += 2) {
		for (int i = 0; i < 4; i++) {
			unpack_line(f, raw, (int)row - 1 + i, lines[i]);
		}
		const int *const above[3] = { lines[0], lines[1], lines[2] }, *const below[3] = { lines[1], lines[2], lines[3] };

		bool second = row + 1 < f->height;

		full_line(isp, f, above, row, colour, &p, rgb[0]);
		full_line(isp, f, below, second ? row + 1 : row, colour, &p, rgb[1]);
		nv12_pair_rgb(rgb[0], rgb[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* one line (y) of the sensor demosaiced, as 8-bit R, G, B (the unpacked lines around it in this thread's l) */
static void
demosaic_row(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint32_t row, const int colour[4],
    const struct pipe *p, int *const l[3], uint8_t *rgb)
{
	for (int i = 0; i < 3; i++) {
		unpack_line(f, raw, (int)row - 1 + i, l[i]);
	}
	full_line(isp, f, (const int *const *)l, row, colour, p, rgb);
}

void
isp_render_still_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t x0, y0, cw, ch, fw = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[3];
	static __thread uint8_t *rows[2], *out[2];
	static __thread uint32_t have_fw, have_width;
	int32_t cached[2] = { -1, -1 };

	if (fw > have_fw) {
		for (int i = 0; i < 3; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * fw);
		}
		for (int i = 0; i < 2; i++) {
			free(rows[i]);
			rows[i] = malloc(3 * (size_t)fw);
		}
		have_fw = fw;
	}
	if (width > have_width) {
		for (int i = 0; i < 2; i++) {
			free(out[i]);
			out[i] = malloc(3 * (size_t)width);
		}
		have_width = width;
	}
	if (lines[2] == NULL || rows[1] == NULL || out[1] == NULL) {
		return;
	}
	crop(isp, f, width, height, &x0, &y0, &cw, &ch);
	cfa(f->bayer, colour);
	pipe_init(isp, &p);
	float sx_step = 2.0f * cw / width, sy_step = 2.0f * ch / height;

	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		for (int k = 0; k < 2; k++) {
			uint32_t r = row + k < height ? row + k : row, at = isp->turned ? height - 1 - r : r;
			float sy = 2.0f * y0 + (at + 0.5f) * sy_step - 0.5f, ty;
			uint32_t sy0 = sy < 0 ? 0 : (uint32_t)sy, sy1, slot[2];

			sy0 = sy0 > f->height - 1 ? f->height - 1 : sy0;
			sy1 = sy0 + 1 > f->height - 1 ? f->height - 1 : sy0 + 1;
			ty = sy < (float)sy0 ? 0 : sy - sy0;
			/* the two lines of the sensor it is between, demosaiced (kept from the line before where they are the same) */
			for (int n = 0; n < 2; n++) {
				uint32_t want = n ? sy1 : sy0;

				if (cached[0] == (int32_t)want || cached[1] == (int32_t)want) {
					slot[n] = cached[0] == (int32_t)want ? 0 : 1;
					continue;
				}
				/* (into the slot the other line does not need: sy0's for sy1, and sy1's kept for sy0) */
				slot[n] = n == 1 ? slot[0] ^ 1 : cached[0] == (int32_t)sy1 ? 1 : 0;
				demosaic_row(isp, f, raw, want, colour, &p, lines, rows[slot[n]]);
				cached[slot[n]] = (int32_t)want;
			}
			const uint8_t *a = rows[slot[0]], *b = rows[slot[1]];
			uint8_t *o = out[k];

			for (uint32_t x = 0; x < width; x++) {
				float sx = 2.0f * x0 + (x + 0.5f) * sx_step - 0.5f, tx;
				uint32_t sx0 = sx < 0 ? 0 : (uint32_t)sx, sx1;

				sx0 = sx0 > fw - 1 ? fw - 1 : sx0;
				sx1 = sx0 + 1 > fw - 1 ? fw - 1 : sx0 + 1;
				tx = sx < (float)sx0 ? 0 : sx - sx0;
				uint8_t *px = o + 3 * (isp->turned ? width - 1 - x : x);

				for (int c = 0; c < 3; c++) {
					float top = a[3 * sx0 + c] + tx * (a[3 * sx1 + c] - a[3 * sx0 + c]);
					float bottom = b[3 * sx0 + c] + tx * (b[3 * sx1 + c] - b[3 * sx0 + c]);

					px[c] = (uint8_t)(top + ty * (bottom - top) + 0.5f);
				}
			}
		}
		bool second = row + 1 < height;

		nv12_pair_rgb(out[0], out[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* ---- lens shading from a flat field --------------------------------------------------------------------- */

bool
isp_lsc_from_flat(struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, float *min_ratio)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2, half_w = cells_w / (LSC_W - 1) / 2, half_h = cells_h / (LSC_H - 1) / 2;
	float level[3][LSC_H][LSC_W], top[3] = { 0, 0, 0 };
	uint32_t clipped = 0, seen = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int cx0 = (int)((uint64_t)gx * (cells_w - 1) / (LSC_W - 1)) - (int)half_w;
			int cy0 = (int)((uint64_t)gy * (cells_h - 1) / (LSC_H - 1)) - (int)half_h;
			double sum[3] = { 0, 0, 0 };
			uint32_t n = 0;

			for (int cy = cy0; cy <= cy0 + 2 * (int)half_h; cy += 2) {
				for (int cx = cx0; cx <= cx0 + 2 * (int)half_w; cx += 2) {
					int v[4];

					if (cx < 0 || cy < 0 || cx >= (int)cells_w || cy >= (int)cells_h) {
						continue;
					}
					cell(raw + (size_t)cy * 2 * f->stride, f->stride, (uint32_t)cx, v);
					sum[0] += v[r];
					sum[2] += v[b];
					for (int j = 0; j < 4; j++) {
						sum[1] += j != r && j != b ? v[j] / 2.0 : 0;
						clipped += v[j] > WHITE - 30;
					}
					seen += 4;
					n++;
				}
			}
			for (int c = 0; c < 3; c++) {
				level[c][gy][gx] = n ? (float)(sum[c] / n) : 0;
				top[c] = level[c][gy][gx] > top[c] ? level[c][gy][gx] : top[c];
			}
		}
	}
	/* bright enough to tell shading from noise, and not clipped anywhere that counts */
	if (top[1] < 100 || clipped * 1000 > seen) {
		return false;
	}
	float darkest = 1;

	for (int c = 0; c < 3; c++) {
		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				float l = level[c][gy][gx] > 1 ? level[c][gy][gx] : 1;

				isp->lsc.gain[c][gy][gx] = top[c] / l;
				if (c == 1 && l / top[c] < darkest) {
					darkest = l / top[c];
				}
			}
		}
	}
	isp->lsc.on = true;
	if (min_ratio != NULL) {
		*min_ratio = darkest;
	}
	return true;
}

/* one module's part of a tuning file (tools/camtuning.py): its ranges, and where each range's values are */
static bool
tune_set(const uint8_t **p, const uint8_t *end, struct isp_tune_set *set, size_t floats)
{
	uint32_t n;

	if (end - *p < 4) {
		return false;
	}
	memcpy(&n, *p, 4);
	*p += 4;
	if (n == 0 || n > TUNE_LIGHTS) {
		return false;
	}
	set->lights = n;
	for (uint32_t i = 0; i < n; i++) {
		if (end - *p < 12) {
			return false;
		}
		memcpy(&set->light[i].lux_from, *p, 4);
		memcpy(&set->light[i].lux_to, *p + 4, 4);
		memcpy(&set->light[i].ccts, *p + 8, 4);
		*p += 12;
		if (set->light[i].ccts == 0 || set->light[i].ccts > TUNE_CCTS) {
			return false;
		}
		for (uint32_t j = 0; j < set->light[i].ccts; j++) {
			if ((size_t)(end - *p) < (2 + floats) * 4) {
				return false;
			}
			memcpy(&set->light[i].cct_from[j], *p, 4);
			memcpy(&set->light[i].cct_to[j], *p + 4, 4);
			set->light[i].v[j] = (const float *)(const void *)(*p + 8);     /* (every field is 4 bytes) */
			*p += (2 + floats) * 4;
		}
	}
	return true;
}

bool
isp_load_tuning(struct isp *isp, const char *path)
{
	struct isp_tuning *t = calloc(1, sizeof(*t));
	int fd = open(path, O_RDONLY);
	struct stat st;
	const uint8_t *p, *end;
	bool good = false;

	if (isp->tuning != NULL) {
		free(isp->tuning->file);
		free(isp->tuning);
		isp->tuning = NULL;
	}
	if (t != NULL && fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 8 && st.st_size < (16 << 20) &&
	    (t->file = malloc((size_t)st.st_size)) != NULL && read(fd, t->file, (size_t)st.st_size) == st.st_size) {
		p = t->file;
		end = p + st.st_size;
		good = memcmp(p, "RWCISP02", 8) == 0;
		p += 8;
		good = good && tune_set(&p, end, &t->ccm, 9) && tune_set(&p, end, &t->lsc, 4 * LSC_W * LSC_H) && end - p >= 4;
		if (good) {
			memcpy(&t->refs, p, 4);
			p += 4;
			good = t->refs <= 16 && (size_t)(end - p) >= t->refs * 12;
		}
		if (good) {
			memcpy(t->ref, p, t->refs * 12);
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (!good) {
		if (t != NULL) {
			free(t->file);
		}
		free(t);
		return false;
	}
	isp->tuning = t;
	return true;
}

/*
 * Where x falls among ascending ranges [from[i], to[i]]: in one (a = b), or between two (a, b and how
 * far from a's end to b's start, 0 to 1); before the first, the first; after the last, the last.
 */
static void
pick(const float *from, const float *to, uint32_t n, float x, uint32_t *a, uint32_t *b, float *t)
{
	*a = *b = n - 1;
	*t = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (x <= to[i]) {
			*a = *b = i;
			if (i > 0 && x < from[i]) {
				*a = i - 1;
				*t = (x - to[i - 1]) / (from[i] - to[i - 1]);
			}
			return;
		}
	}
}

/* a module's values for this light and temperature: those of the ranges they fall in, or between */
static void
tune_blend(const struct isp_tune_set *set, float lux_index, float cct, size_t floats, float *out)
{
	float from[TUNE_LIGHTS], to[TUNE_LIGHTS], tl;
	uint32_t la, lb;

	for (uint32_t i = 0; i < set->lights; i++) {
		from[i] = set->light[i].lux_from;
		to[i] = set->light[i].lux_to;
	}
	pick(from, to, set->lights, lux_index, &la, &lb, &tl);
	memset(out, 0, floats * sizeof(float));
	for (int k = 0; k < 2; k++) {
		uint32_t l = k ? lb : la, ca, cb;
		float w = k ? tl : 1 - tl, tc;

		if (w == 0) {
			continue;
		}
		pick(set->light[l].cct_from, set->light[l].cct_to, set->light[l].ccts, cct, &ca, &cb, &tc);
		for (size_t i = 0; i < floats; i++) {
			out[i] += w * ((1 - tc) * set->light[l].v[ca][i] + tc * set->light[l].v[cb][i]);
		}
	}
}

void
isp_tune(struct isp *isp, float lux_index, float cct)
{
	const struct isp_tuning *t = isp->tuning;
	float m[9], g[4 * LSC_H * LSC_W];
	const int grid = LSC_H * LSC_W;

	if (t == NULL) {
		return;
	}
	tune_blend(&t->ccm, lux_index, cct, 9, m);
	isp_set_ccm(isp, m);
	tune_blend(&t->lsc, lux_index, cct, 4 * (size_t)grid, g);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int at = gy * LSC_W + gx;

			isp->lsc.gain[0][gy][gx] = g[at];
			isp->lsc.gain[1][gy][gx] = (g[grid + at] + g[2 * grid + at]) / 2;
			isp->lsc.gain[2][gy][gx] = g[3 * grid + at];
		}
	}
	isp->lsc.on = true;
}
