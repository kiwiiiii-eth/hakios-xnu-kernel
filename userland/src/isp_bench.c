/*
 * How long the camera's processor (src/camera_isp.c) takes over a picture on one thread, outside mediaserverd: a frame
 * made up here (RAW10, a gradient with some noise), the preview's 1504x1128 made of it as the front camera's binned
 * 2304x1728 and as the main camera's whole 4656x3496, with no colour tuning and with the image's own (fw/camera-*.isp).
 * Run beside the Mac (cc -O2 isp_bench.c camera_isp.c): libhakios.media took 240-320 ms of processor time a preview
 * frame on the OnePlus 6T where this Mac takes 7 on one core, while src/cpu_test.c says the 6T's cores are 2.3 times
 * slower, no more -- whether it is this code on these cores, or something about mediaserverd's.
 *
 * usage: isp_bench [TUNING.isp]
 */
#include <mach/mach_time.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_isp.h"

static double
ms_since(uint64_t t0)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

/* RAW10, four pixels in five bytes: a gradient across, some noise, the black level 64 under it */
static uint8_t *
made_up(uint32_t width, uint32_t height, struct hakios_camera_frame *f)
{
	uint32_t stride = width * 5 / 4;
	uint8_t *raw = malloc((size_t)stride * height);
	uint32_t seed = 12345;

	memset(f, 0, sizeof(*f));
	f->header_bytes = sizeof(*f);
	f->width = width;
	f->height = height;
	f->stride = stride;
	f->format = 1;
	f->black = 64;
	f->gain_unity = 256;
	f->gain = 256;
	for (uint32_t y = 0; raw != NULL && y < height; y++) {
		uint8_t *line = raw + (size_t)y * stride;

		for (uint32_t x = 0; x < width; x += 4) {
			uint16_t v[4];
			uint8_t *g = line + x / 4 * 5;

			for (int k = 0; k < 4; k++) {
				seed = seed * 1103515245 + 12345;
				v[k] = (uint16_t)(64 + (x + k) * 600 / width + (y & 64 ? 120 : 0) + (seed >> 24) % 32);
				g[k] = (uint8_t)(v[k] >> 2);
			}
			g[4] = (uint8_t)((v[0] & 3) | (v[1] & 3) << 2 | (v[2] & 3) << 4 | (v[3] & 3) << 6);
		}
	}
	return raw;
}

#ifdef ISP_PROFILE
extern uint64_t isp_profile[6];
#endif

static void
bench(const char *what, struct isp *isp, uint32_t sw, uint32_t sh)
{
	struct hakios_camera_frame f;
	uint8_t *raw = made_up(sw, sh, &f), *out = malloc(1504 * 1128 * 3 / 2);
	double best = 1e9;

	if (raw == NULL || out == NULL) {
		printf("isp_bench: no memory\n");
		return;
	}
#ifdef ISP_PROFILE
	memset(isp_profile, 0, sizeof(isp_profile));
#endif
	for (int i = 0; i < 5; i++) {
		uint64_t t0 = mach_absolute_time();
		double t;

		isp_render_nv12(isp, &f, raw, out, 1504, out + 1504 * 1128, 1504, 1504, 1128, 0, 1128, false);
		t = ms_since(t0);
		best = t < best ? t : best;
	}
	printf("isp_bench: 1504x1128 of %ux%u, %s: %.1f ms on one thread (the best of 5)\n", sw, sh, what, best);
#ifdef ISP_PROFILE
	{
		mach_timebase_info_data_t tb;

		mach_timebase_info(&tb);
		double k = (double)tb.numer / tb.denom / 1e6 / 5;

		printf("isp_bench:   a picture's stages: unpack %.1f ms, shading+matrix+gamma %.1f, across %.1f, between lines %.1f, NV12 "
		    "%.1f\n", isp_profile[0] * k, isp_profile[1] * k, isp_profile[2] * k, isp_profile[3] * k, isp_profile[4] * k);
		memset(isp_profile, 0, sizeof(isp_profile));
	}
#endif
	free(raw);
	free(out);
}

/* the same picture in pieces on THREADS threads at once, as libhakios.media makes it: the time, and the processor time */
#define THREADS 7
struct piece { struct isp *isp; struct hakios_camera_frame *f; uint8_t *raw, *out; uint32_t first, end; uint64_t cpu_ns; };

static void *
piece_thread(void *arg)
{
	struct piece *p = arg;
	uint64_t t0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);

	for (int i = 0; i < 5; i++) {
		isp_render_nv12(p->isp, p->f, p->raw, p->out, 1504, p->out + 1504 * 1128, 1504, 1504, 1128, p->first, p->end, false);
	}
	p->cpu_ns = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - t0;
	return NULL;
}

static void
bench_threads(const char *what, struct isp *isp, uint32_t sw, uint32_t sh)
{
	struct hakios_camera_frame f;
	uint8_t *raw = made_up(sw, sh, &f), *out = malloc(1504 * 1128 * 3 / 2);
	struct piece pieces[THREADS];
	pthread_t threads[THREADS];
	uint64_t t0 = mach_absolute_time(), cpu = 0;
	uint32_t rows = (1128 / THREADS + 1) & ~1u;

	for (int i = 0; i < THREADS; i++) {
		pieces[i] = (struct piece){ isp, &f, raw, out, (uint32_t)i * rows, i + 1 == THREADS ? 1128 : (uint32_t)(i + 1) * rows, 0 };
		pthread_create(&threads[i], NULL, piece_thread, &pieces[i]);
	}
	for (int i = 0; i < THREADS; i++) {
		pthread_join(threads[i], NULL);
		cpu += pieces[i].cpu_ns;
	}
	double wall = ms_since(t0) / 5;

	printf("isp_bench: 1504x1128 of %ux%u, %s, on %d threads: %.1f ms a picture, %.1f ms of processor time (%.1f processors' "
	    "worth)\n", sw, sh, what, THREADS, wall, cpu / 5 / 1e6, cpu / 5 / 1e6 / wall);
	free(raw);
	free(out);
}

int
main(int argc, char **argv)
{
	struct isp isp;
	uint64_t t0;
	volatile float sink = 0;
	volatile uint8_t sink8 = 0;

	isp_init(&isp);
	bench("no tuning", &isp, 2304, 1728);
	bench("no tuning", &isp, 4656, 3496);
	if (argc > 1 && isp_load_tuning(&isp, argv[1])) {
		isp_tune(&isp, 300, 4000);
		bench("tuned", &isp, 2304, 1728);
		bench("tuned", &isp, 4656, 3496);
		bench_threads("tuned", &isp, 2304, 1728);
		bench_threads("tuned", &isp, 4656, 3496);
	}
	/* the kinds of arithmetic the processor does a lot of, 20 million times each */
	t0 = mach_absolute_time();
	for (int i = 0; i < 20000000; i++) {
		sink = sink * 0.999f + (float)(i & 1023) * 0.5f;
	}
	printf("isp_bench: 20M float multiply-adds: %.1f ms\n", ms_since(t0));
	t0 = mach_absolute_time();
	for (int i = 0; i < 20000000; i++) {
		float v = (float)(i & 4095) * 0.93f + 0.5f;

		sink8 = isp.gamma[(int)v > 4095 ? 4095 : (int)v];
	}
	printf("isp_bench: 20M float-to-int conversions and table look-ups: %.1f ms\n", ms_since(t0));
	(void)sink;
	(void)sink8;
	return 0;
}
