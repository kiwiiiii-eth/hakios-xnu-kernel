/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of Darwin userland 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (Darwin userland 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and Darwin knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/*
 * Boot-arg fbsubtype=N with fbscale and fbwidth/fbheight: a screen of another iPhone's class -- its pixels (fbwidth x
 * fbheight, as given, not 375 x 812 points at fbscale), its scale and its artwork-device-subtype (the device tree's
 * /product, what MobileGestalt's screen class, corner radius and artwork come from). UIKit picks the status bar's look
 * by width, scale and corner radius together and has none for the iPhone X's 375 points at 2 ("No status bar visual
 * provider ... rdar://45025538", a red bar), and PosterBoard drew no wallpaper there (2 November). fbwidth=828
 * fbheight=1792 fbscale=2 fbsubtype=1792 is the iPhone XR's: 1.85 times fewer pixels than this one's to draw.
 */
static TUNABLE(uint32_t, gFBSubtype, "fbsubtype", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had Darwin killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 2);
/*
 * fb_filter=2 (the default): nearest neighbour while frames come, bilinear once they stop. Bilinear costs: a frame
 * took 3.6 ms to scale onto the panel as a rule and 11 ms for one in ten, at any priority (fb_pri), against 1.7 and
 * 4.3 by nearest neighbour (touch_lat=1, 30 October) -- and at 120 Hz a refresh is 8.3 ms. What nearest neighbour
 * gets wrong (a column in 25 and a row in 67 left out) does not show while the picture moves; so a frame drawn so is
 * drawn again, filtered, when no other has come for SETTLE_MS: what stands still on the screen is the bilinear one.
 *
 * Frames that come slowly are filtered at once. The home screen's clock asks for 10 frames a second, and each was drawn
 * by nearest neighbour and 40 ms later by bilinear: 51 frames and 51 drawn again in 5 seconds, every edge of a still screen
 * moving back and forth (4 November, "the screen jerks all the time"). Nearest neighbour is for FAST_FRAMES in a row and on.
 */
#define SETTLE_MS       40
#define FAST_FRAMES     2
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
/*
 * Boot-arg fb_ca_hz=N: the refresh rate told to CoreAnimation (IOMFBDisplayRefresh) when it is not to be fb_hz's. On
 * this system CoreAnimation draws at half the rate it is told unless something asks it for more: told 120 it gave 60
 * (tools/frames.py: frames every 16.7 ms, with or without CADisableMinimumFrameDurationOnPhone, CA_FORCE_120HZ and
 * CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0, 30 October). The refreshes themselves still come at fb_hz.
 */
static TUNABLE(uint32_t, gCARefreshHz, "fb_ca_hz", 0);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_pri=N: the scheduling priority of the threads that put frames on the panel (the presenter and its
 * helpers); 0 is the kernel's default for its threads. By default 92 (BASEPRI_PREEMPT): at the kernel's they lost the
 * processors to the window server's own real-time threads under load, and a frame took 3.6 ms to put on the panel
 * as a rule but 10.9 for one in ten (touch_lat=1, 30 October), more than the whole of a refresh at 120 Hz.
 */
static TUNABLE(uint32_t, gThreadPriority, "fb_pri", 92);
extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void * parameter, integer_t priority,
    thread_t * new_thread);
/* a thread of the panel's, at fb_pri */
static kern_return_t
panel_thread_start(thread_continue_t continuation, void * parameter, thread_t * thread)
{
	if (gThreadPriority >= 1 && gThreadPriority <= 95) {
		return kernel_thread_start_priority(continuation, parameter, (integer_t) gThreadPriority, thread);
	}
	return kernel_thread_start(continuation, parameter, thread);
}
/* kern.hakios.fb_surface: the IOSurface on the panel, for a picture of what is there (SpringBoard's wallpaper_freeze=1) */
SYSCTL_DECL(_kern_hakios);
static uint32_t gShownSurface;
SYSCTL_UINT(_kern_hakios, OID_AUTO, fb_surface, CTLFLAG_RD | CTLFLAG_LOCKED, &gShownSurface, 0, "the IOSurface last put on the panel");

#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: drivers/input/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

/*
 * Boot-arg fb_measure=1: how often the panel is really refreshed. The boot loader leaves the display controller running
 * and this driver only writes into the memory it scans, so fb_hz says how often frames are made, not how often the
 * panel shows one. The frame counter of the controller's DSI interface (SM7325's DPU: INTF_1 at 0x0ae36000,
 * INTF_FRAME_COUNT at 0xac, as Linux's catalogue for sc7280 has them) is read twice, a second apart, and the
 * difference printed. Reads only, once, a minute after the driver starts (what the kernel says in its first minute
 * does not come back with the phone's log), in a thread of its own; off by default.
 */
static TUNABLE(uint32_t, gMeasure, "fb_measure", 0);
#define DPU_SC7280_INTF1        0x0ae36000UL            /* SM7325 (sc7280): INTF_1, what the panel of redwood is on */
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0                   /* the line the interface is scanning (Linux's dpu_hw_intf.c) */
/*
 * Boot-arg fb_beam=0: frames written to the panel's memory whenever their beat comes, as before. The panel reads the
 * very memory this driver writes (one buffer, and it takes the scan 8.3 ms to read it, this driver 3 to write it): a
 * frame written across the line being read came out torn, a piece of the last frame and a piece of this one, moving
 * bands with doubled icons and text wherever something moved (3 November, seen on the phone and in screenshots).
 * Now each thread's share of the rows waits for the scan to have passed it and is written behind the line being read,
 * in time for the next scan to read all of the new frame: that scan shows the whole frame, nothing is ever read half
 * written. The line comes from the interface's own counter; reads only. It is looked at a minute after the driver
 * starts, with fb_measure's reads, and if it does not count as a line counter should, frames stay on their beat.
 */
static TUNABLE(uint32_t, gBeam, "fb_beam", 1);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
static inline uint32_t refreshHz(void);

/*
 * Boot-arg fb_intf=PHYSICAL: the registers of the display interface the panel is on, for a port. Without it the SoC the kernel
 * was configured for says (hakios_kver, CONFIG_SOC): SM7325's address read on another SoC is somebody else's registers, or an
 * abort a minute after boot, so on a SoC not known here nothing is read and frames stay on their beat.
 */
static TUNABLE(uint64_t, gIntfPhysical, "fb_intf", 0);

static vm_offset_t
panelInterface(void)
{
	char version[128] = "";
	uint64_t physical = gIntfPhysical;

	if (physical == 0 && PE_parse_boot_argn("hakios_kver", version, sizeof(version)) && strnstr(version, "-sc7280-", sizeof(version)) != NULL) {
		physical = DPU_SC7280_INTF1;
	}
	return physical != 0 ? ml_io_map((vm_offset_t) (physical & ~0x3fffULL), 0x4000) + (vm_offset_t) (physical & 0x3fffULL) : 0;
}

static void
measurePanel(__unused void * arg, __unused wait_result_t wr)
{
	vm_offset_t intf;
	uint32_t on, first, second;

	IOSleep(60000);
	intf = panelInterface();
	if (intf == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_measure: no display interface known for this SoC (fb_intf=)\n");
		return;
	}
	on = *(volatile uint32_t *)(intf + INTF_TIMING_ENGINE_EN);
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	IOSleep(1000);
	second = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	IOLog("MiniIOMobileFramebuffer: the panel: DSI interface's timing engine %s, %u refreshes in one second "
	    "(frames are made at %u a second: fb_hz)\n", (on & 1) ? "on" : "OFF", second - first, refreshHz());
}

/*
 * Boot-arg mdp_probe=1 (SDM845: the OnePlus 6T): how the boot loader left the display controller, read once a minute after
 * the driver starts, in a thread of its own; reads only, nothing written. This driver only writes into the memory the
 * controller scans; to let the controller scale and flip instead (as on the same SoC Bergamota's Poco F1 does: a VIG
 * pipe's QSEED3 scaler, page flipping in the continuous splash region), what is there has to be known first: which CTL
 * and layer mixer are in use, which pipe scans which address at what size and format, whether that pipe has a scaler,
 * whether the panel is driven in video mode (INTF_1's timing engine) or command mode (a ping-pong block's auto-refresh),
 * and whether the apps SMMU translates the display's streams (0x880, 0xc80) or lets them through. The addresses are
 * Linux's sdm845 DPU catalogue's (mdp at 0x0ae01000; ctl +0x1000, sspp VIG0 +0x4000 and DMA0 +0x24000 every 0x2000,
 * lm +0x44000, intf +0x6a000, pingpong +0x70000), only numbers.
 */
static TUNABLE(uint32_t, gMdpProbe, "mdp_probe", 0);
extern "C" unsigned long long ml_phys_read_double_64(uint64_t paddr);

static vm_offset_t
mdpMap(uint64_t physical, uint32_t size)
{
	vm_offset_t va = ml_io_map((vm_offset_t) (physical & ~0x3fffULL), (size + (physical & 0x3fffULL) + 0x3fffULL) & ~0x3fffULL);

	return va != 0 ? va + (vm_offset_t) (physical & 0x3fffULL) : 0;
}

#define MDPR(base, off) (*(volatile uint32_t *)((base) + (off)))

static void
mdpProbe(__unused void * arg, __unused wait_result_t wr)
{
	vm_offset_t low, dma, lm, intf, pp, smmu;
	char version[128] = "";

	IOSleep(60000);
	if (!PE_parse_boot_argn("hakios_kver", version, sizeof(version)) || strnstr(version, "-sdm845-", sizeof(version)) == NULL) {
		printf("MiniIOMobileFramebuffer: mdp_probe: not an SDM845 kernel: nothing read\n");
		return;
	}
	low = mdpMap(0x0ae00000ULL, 0xc000);            /* MDSS, MDP top, CTL0..4, VIG0..3 */
	dma = mdpMap(0x0ae24000ULL, 0x8000);            /* DMA0..3 */
	lm = mdpMap(0x0ae44000ULL, 0x4000);             /* LM0..LM3 at 0x45000.. */
	intf = mdpMap(0x0ae68000ULL, 0x8000);           /* INTF0..2 at 0x0ae6b000, every 0x800 */
	pp = mdpMap(0x0ae70000ULL, 0x4000);             /* PINGPONG0..3 at 0x71000.. */
	smmu = mdpMap(0x15000000ULL, 0x4000);           /* the apps SMMU's global registers */
	if (low == 0 || dma == 0 || lm == 0 || intf == 0 || pp == 0) {
		printf("MiniIOMobileFramebuffer: mdp_probe: could not map the display controller\n");
		return;
	}
	printf("mdp_probe: MDSS version %#x, MDP top version %#x, DISP_INTF_SEL %#x, SPLIT %#x\n", MDPR(low, 0x0),
	    MDPR(low, 0x1000), MDPR(low, 0x1004), MDPR(low, 0x12f4));
	for (int c = 0; c < 5; c++) {
		vm_offset_t ctl = low + 0x2000 + 0x200 * c;

		printf("mdp_probe: CTL%d layer LM0..LM3 %#x %#x %#x %#x, layer ext %#x %#x, top %#x, flush %#x, start %#x\n", c,
		    MDPR(ctl, 0x00), MDPR(ctl, 0x04), MDPR(ctl, 0x08), MDPR(ctl, 0x0c), MDPR(ctl, 0x40), MDPR(ctl, 0x44),
		    MDPR(ctl, 0x14), MDPR(ctl, 0x18), MDPR(ctl, 0x1c));
	}
	for (int k = 0; k < 8; k++) {
		vm_offset_t sspp = k < 4 ? low + 0x5000 + 0x2000 * k : dma + 0x1000 + 0x2000 * (k - 4);

		printf("mdp_probe: %s%d src size %#x img %#x xy %#x, out size %#x xy %#x, addr %#x %#x, stride %#x %#x, format %#x, "
		    "unpack %#x, op mode %#x, fetch %#x%s", k < 4 ? "VIG" : "DMA", k & 3, MDPR(sspp, 0x00), MDPR(sspp, 0x04),
		    MDPR(sspp, 0x08), MDPR(sspp, 0x0c), MDPR(sspp, 0x10), MDPR(sspp, 0x14), MDPR(sspp, 0x18), MDPR(sspp, 0x24),
		    MDPR(sspp, 0x28), MDPR(sspp, 0x30), MDPR(sspp, 0x34), MDPR(sspp, 0x38), MDPR(sspp, 0x6c), k < 4 ? "" : "\n");
		if (k < 4) {
			printf(", qseed3 version %#x op mode %#x, steps %#x %#x\n", MDPR(sspp, 0xa00), MDPR(sspp, 0xa04),
			    MDPR(sspp, 0xa10), MDPR(sspp, 0xa14));
		}
	}
	for (int m = 0; m < 4; m++) {
		vm_offset_t mixer = lm + 0x1000 * (m + 1);        /* LM0 at 0x0ae45000 */

		if (m >= 3) {
			break;                          /* (LM0..LM2 in the mapped window) */
		}
		printf("mdp_probe: LM%d op mode %#x, out size %#x, border %#x %#x, stage0 op %#x\n", m, MDPR(mixer, 0x00),
		    MDPR(mixer, 0x04), MDPR(mixer, 0x08), MDPR(mixer, 0x10), MDPR(mixer, 0x20));
	}
	for (int i = 0; i < 3; i++) {
		vm_offset_t in = intf + 0x3000 + 0x800 * i;     /* INTF0 at 0x0ae6b000 */
		uint32_t f1, f2;

		f1 = MDPR(in, 0x0ac);
		IOSleep(200);
		f2 = MDPR(in, 0x0ac);
		printf("mdp_probe: INTF%d timing engine %#x, frame count %u -> %u in 200 ms, line %u, mux %#x\n", i, MDPR(in, 0x000),
		    f1, f2, MDPR(in, 0x0b0), MDPR(in, 0x2c));
	}
	for (int q = 0; q < 4; q++) {
		vm_offset_t ping = pp + 0x1000 + 0x800 * q;
		uint32_t l1, l2;

		l1 = MDPR(ping, 0x02c);
		IOSleep(5);
		l2 = MDPR(ping, 0x02c);
		printf("mdp_probe: PINGPONG%d tear check %#x, vsync %#x, height %#x, autorefresh %#x, line count %u then %u\n", q,
		    MDPR(ping, 0x000), MDPR(ping, 0x004), MDPR(ping, 0x008), MDPR(ping, 0x030), l1, l2);
	}
	if (smmu != 0) {
		uint32_t idr0 = MDPR(smmu, 0x20), groups = idr0 & 0xff;

		printf("mdp_probe: apps SMMU: IDR0 %#x, %u stream match groups; the display's streams:\n", idr0, groups);
		for (uint32_t n = 0; n < groups && n < 128; n++) {
			uint32_t smr = MDPR(smmu, 0x800 + 4 * n), s2cr = MDPR(smmu, 0xc00 + 4 * n);
			uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

			if ((smr & 0x80000000u) && ((0x880 & ~mask) == (id & ~mask) || (0xc80 & ~mask) == (id & ~mask))) {
				printf("mdp_probe:   SMR%u id %#x mask %#x -> S2CR %#x (type %u: %s, context bank %u)\n", n, id, mask, s2cr,
				    (s2cr >> 16) & 3, ((s2cr >> 16) & 3) == 1 ? "BYPASS" : ((s2cr >> 16) & 3) == 0 ? "translate" : "fault",
				    s2cr & 0xff);
			}
		}
	}
	if (smmu != 0) {
		/*
		 * The context bank the display's streams go through: its registers, and its page tables walked (reads of physical
		 * memory only) over the continuous splash region, 0x9d400000 and 36 MiB, and a little past it: which of it the
		 * display controller can fetch. A fetch the tables do not map is a fault, and may reset the SoC.
		 */
		uint32_t idr1 = MDPR(smmu, 0x24), page = (idr1 >> 31) ? 0x10000 : 0x1000, pages = 1u << (((idr1 >> 28) & 7) + 1);
		uint32_t s2cr = MDPR(smmu, 0xc00), bank = s2cr & 0xff;
		vm_offset_t gr1 = mdpMap(0x15000000ULL + page, 0x1000);
		vm_offset_t cb = mdpMap(0x15000000ULL + (uint64_t) page * pages + (uint64_t) page * bank, 0x1000);

		if (gr1 != 0 && cb != 0) {
			uint32_t cbar = MDPR(gr1, 4 * bank), cba2r = MDPR(gr1, 0x800 + 4 * bank), sctlr = MDPR(cb, 0x00);
			uint32_t tcr = MDPR(cb, 0x30), tcr2 = MDPR(cb, 0x10), mair0 = MDPR(cb, 0x38);
			uint64_t ttbr0 = (uint64_t) MDPR(cb, 0x20) | (uint64_t) MDPR(cb, 0x24) << 32;
			bool va64 = cba2r & 1;
			uint32_t t0sz = va64 ? (tcr & 0x3f) : (tcr & 7), vabits = (va64 ? 64 : 32) - t0sz;

			printf("mdp_probe: context bank %u (page %#x, %u global pages): CBAR %#x CBA2R %#x SCTLR %#x TCR %#x TCR2 %#x MAIR0 %#x "
			    "TTBR0 %#llx, %s, %u-bit input\n", bank, page, pages, cbar, cba2r, sctlr, tcr, tcr2, mair0, ttbr0,
			    va64 ? "AArch64 tables" : "LPAE tables", vabits);
			if ((sctlr & 1) && ((va64 ? (tcr >> 14) & 3 : 0) == 0) && vabits >= 25 && vabits <= 39) {
				uint64_t base = ttbr0 & 0x0000fffffffff000ULL;
				uint64_t runVA = 0, runPA = 0, runLen = 0;
				int start = vabits > 30 ? 1 : 2;

				for (uint64_t va = 0x9d400000ULL; va < 0x9fc00000ULL; va += 0x1000) {
					uint64_t table = base, pa = ~0ULL;

					for (int level = start; level <= 3; level++) {
						int shift = 39 - 9 * level;
						uint64_t index = (va >> shift) & 0x1ff, d;

						if (level == start) {
							index = (va >> shift) & ((1ULL << (vabits - shift)) - 1);
						}
						d = ml_phys_read_double_64(table + 8 * index);
						if (!(d & 1)) {
							break;
						}
						if (level < 3 && (d & 2)) {
							table = d & 0x0000fffffffff000ULL;
							continue;
						}
						if (level == 3 && !(d & 2)) {
							break;
						}
						pa = (d & 0x0000fffffffff000ULL & ~((1ULL << shift) - 1)) | (va & ((1ULL << shift) - 1));
						break;
					}
					if (runLen != 0 && pa == runPA + runLen && va == runVA + runLen) {
						runLen += 0x1000;
						continue;
					}
					if (runLen != 0) {
						printf("mdp_probe:   %#llx..%#llx -> %#llx (%llu KB)\n", runVA, runVA + runLen, runPA, runLen >> 10);
					}
					if (pa == ~0ULL) {
						runLen = 0;
						if ((va & 0xfffff) == 0) {
							printf("mdp_probe:   %#llx: not mapped\n", va);
						}
						continue;
					}
					runVA = va;
					runPA = pa;
					runLen = 0x1000;
				}
				if (runLen != 0) {
					printf("mdp_probe:   %#llx..%#llx -> %#llx (%llu KB)\n", runVA, runVA + runLen, runPA, runLen >> 10);
				}
			} else {
				printf("mdp_probe:   tables not walked (translation off, a granule other than 4 KB, or an input size not handled)\n");
			}
		}
	}
	printf("mdp_probe: done\n");
}


/*
 * Boot-arg fb_hw=1 (SDM845, the OnePlus 6T; off by default): the display controller scales and flips, not this driver.
 * mdp_probe found the boot chain's picture on VIG0 (the one kind of pipe with a QSEED3 scaler) at 0x9d400000, 1080 x 2340,
 * the panel in command mode with its ping-pong block's auto-refresh (the controller fetches the picture again at every
 * refresh by itself), and the display's SMMU context with translation off (it fetches physical addresses); the boot
 * firmware keeps 0x9d400000 and 36 MiB (the continuous splash) out of everything else's memory (DeviceMemoryMap.h).
 * So: two buffers in that region past the boot picture hold the canvas (fWidth x fHeight, the iPhone's) 1:1; a frame is
 * copied into the one not on the panel with memcpy, row by row (no scaling, no filtering: drawRows' unscaled road); VIG0
 * is pointed at it and the CTL flushed; the controller takes the new address at its next refresh (the flush bits read
 * back 0 then), and QSEED3 scales it, bilinear, to the panel. Until the flush is taken the other buffer is still being
 * read and is not written. Was: every frame scaled onto the panel by the CPU, 8 to 11 ms for the slow ones with the
 * Dynamic Island's 1179 x 2556 (r265), and the panel reading the memory being written (tearing).
 * The registers are Linux's sdm845 DPU catalogue's and dpu_hw_sspp.c / dpu_hw_util.c's (numbers only; mdp at 0x0ae01000,
 * VIG0 +0x4000, its QSEED3 +0xa00, CTL0 +0x1000), as Bergamota's notes for the same SoC have them.
 */
static TUNABLE(uint32_t, gHw, "fb_hw", 0);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
#define HW_SPLASH_PA            0x9d400000ULL
/*
 * The region's last megabyte but one, 0x9f700000, is hakios.dsibl.c's (its DSI command packets: PACKET_PHYS), and the
 * buffers stay below it (r271-r273 had the second one at 0x9ec00000, 11.5 MB long: over the packets' megabyte).
 */
#define HW_SPLASH_END           (0x9d400000ULL + 0x02300000ULL)
#define HW_BUFFER_PA0           0x9de00000ULL           /* past the boot picture (1080 x 2340 x 4 = 10.1 MB, to 0x9dda4000) */
#define HW_BUFFER_PA1           0x9ea00000ULL
#define HW_BUFFER_MAX           0x00c00000ULL           /* 12 MiB each */
#define VIG0_PA                 0x0ae05000ULL
#define CTL0_PA                 0x0ae02000ULL
static vm_offset_t gHwVig, gHwCtl, gHwVA[2];
static uint32_t gHwStride, gHwBack, gHwOn, gHwProgrammed;
static uint64_t gHwFlips, gHwLatchWait, gHwLatchMax, gHwTimeouts, gHwCopy;

/*
 * A panic's text is drawn into the boot picture (0x9d400000, the console's), which with fb_hw the controller no longer
 * scans. So VIG0's registers as the boot chain left them are kept before anything is written, and the panic hook
 * (PE_arm_debug_panic_hook, a DEVELOPMENT kernel's) writes them back and flushes, with plain stores only (no lock, no
 * allocation, no log), before the hook that was there: the panel shows the boot picture, and the text, from the next
 * refresh on.
 */
static const uint16_t gHwSavedOffsets[] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x24, 0x100, 0x104, 0x108, 0x110, 0x114, 0x118,
	                                    0x120, 0x124, 0x128, 0xa10, 0xa14, 0xa18, 0xa1c, 0xa20, 0xa40, 0xa44, 0xa48,
	                                    0xa90, 0xa94, 0xa98, 0xa9c, 0xa04 /* (the scaler's mode last, as when it was set) */ };
static uint32_t gHwSaved[sizeof(gHwSavedOffsets) / sizeof(gHwSavedOffsets[0])];
extern "C" void (*PE_arm_debug_panic_hook)(const char *str);
static void (*gHwPanicNext)(const char *str);

static void
hwRestore(void)
{
	for (unsigned i = 0; i < sizeof(gHwSavedOffsets) / sizeof(gHwSavedOffsets[0]); i++) {
		*(volatile uint32_t *)(gHwVig + gHwSavedOffsets[i]) = gHwSaved[i];
	}
	*(volatile uint32_t *)(gHwCtl + 0x18) = 1u << 0 /* VIG0 */ | 1u << 17 /* CTL */;
}

static void
hwPanicHook(const char *str)
{
	if (gHwProgrammed) {
		gHwProgrammed = 0;
		hwRestore();
	}
	if (gHwPanicNext != NULL) {
		gHwPanicNext(str);
	}
}

static bool
hwSetup(uint32_t width, uint32_t height, uint32_t panelWidth, uint32_t panelHeight)
{
	char version[128] = "";
	uint64_t size;

	if (gHw == 0) {
		return false;
	}
	if (!PE_parse_boot_argn("hakios_kver", version, sizeof(version)) || strnstr(version, "-sdm845-", sizeof(version)) == NULL) {
		IOLog("MiniIOMobileFramebuffer: fb_hw: not an SDM845 kernel: the CPU scales\n");
		return false;
	}
	gHwStride = (width * 4 + 63) & ~63u;
	size = (uint64_t) gHwStride * height;
	if (width > 2560 || size > HW_BUFFER_MAX || HW_BUFFER_PA1 + size > HW_SPLASH_END || width * 4 > panelWidth * 4 * 4 ||
	    height > panelHeight * 4 || HW_SPLASH_PA + (uint64_t) panelWidth * 4 * panelHeight > HW_BUFFER_PA0) {
		IOLog("MiniIOMobileFramebuffer: fb_hw: a canvas of %ux%u does not fit (stride %u, %llu bytes): the CPU scales\n", width,
		    height, gHwStride, size);
		return false;
	}
	gHwVig = ml_io_map((vm_offset_t) (VIG0_PA & ~0x3fffULL), 0x4000) + (vm_offset_t) (VIG0_PA & 0x3fffULL);
	gHwCtl = ml_io_map((vm_offset_t) (CTL0_PA & ~0x3fffULL), 0x4000) + (vm_offset_t) (CTL0_PA & 0x3fffULL);
	gHwVA[0] = ml_io_map_wcomb((vm_offset_t) HW_BUFFER_PA0, (vm_size_t) ((size + 0x3fff) & ~0x3fffULL));
	gHwVA[1] = ml_io_map_wcomb((vm_offset_t) HW_BUFFER_PA1, (vm_size_t) ((size + 0x3fff) & ~0x3fffULL));
	if (gHwVig < 0x4000 || gHwCtl < 0x4000 || gHwVA[0] == 0 || gHwVA[1] == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_hw: could not map the controller or the buffers: the CPU scales\n");
		return false;
	}
	if (*(volatile uint32_t *)(gHwVig + 0x14) != (uint32_t) HW_SPLASH_PA || *(volatile uint32_t *)(gHwVig + 0xa00) < 0x1002) {
		IOLog("MiniIOMobileFramebuffer: fb_hw: VIG0 scans %#x, its QSEED3 is version %#x: not the boot chain's picture on a "
		    "scaling pipe, the CPU scales\n", *(volatile uint32_t *)(gHwVig + 0x14), *(volatile uint32_t *)(gHwVig + 0xa00));
		return false;
	}
	for (unsigned i = 0; i < sizeof(gHwSavedOffsets) / sizeof(gHwSavedOffsets[0]); i++) {
		gHwSaved[i] = *(volatile uint32_t *)(gHwVig + gHwSavedOffsets[i]);
	}
	gHwPanicNext = PE_arm_debug_panic_hook;
	PE_arm_debug_panic_hook = hwPanicHook;
	bzero((void *) gHwVA[0], (size_t) size);
	bzero((void *) gHwVA[1], (size_t) size);
	IOLog("MiniIOMobileFramebuffer: fb_hw: the controller scales %ux%u (stride %u) to the panel's %ux%u and flips between "
	    "%#llx and %#llx\n", width, height, gHwStride, panelWidth, panelHeight, HW_BUFFER_PA0, HW_BUFFER_PA1);
	gHwOn = 1;
	return true;
}

#define VIGW(off, value) (*(volatile uint32_t *)(gHwVig + (off)) = (value))

/* VIG0 once: the canvas in, the panel out, QSEED3 bilinear between (Linux's dpu_hw_sspp_setup_rects, _pe_config, scaler3) */
static void
hwProgram(uint32_t width, uint32_t height, uint32_t panelWidth, uint32_t panelHeight)
{
	uint32_t src = height << 16 | width, dst = panelHeight << 16 | panelWidth;
	uint32_t stepX = (uint32_t) (((uint64_t) width << 21) / panelWidth), stepY = (uint32_t) (((uint64_t) height << 21) / panelHeight);
	uint32_t alpha = (*(volatile uint32_t *)(gHwVig + 0x30) & (1u << 8)) != 0;
	uint32_t op = 1u /* on */ | 3u << 16 /* Y/RGB: bilinear */ | 1u << 14 /* not DX */ | 1u << 31 /* blend */ |
	    (alpha ? (1u << 10 | 1u << 29) : 0) /* alpha on, bilinear */;

	VIGW(0x00, src);                        /* SRC_SIZE */
	VIGW(0x04, src);                        /* SRC_IMG_SIZE */
	VIGW(0x08, 0);                          /* SRC_XY */
	VIGW(0x0c, dst);                        /* OUT_SIZE */
	VIGW(0x10, 0);                          /* OUT_XY */
	VIGW(0x24, gHwStride);                  /* SRC_YSTRIDE0 */
	for (uint32_t c = 0; c < 3; c++) {      /* pixel extension, none: what is fetched is the source */
		VIGW(0x100 + 0x10 * c, 0);
		VIGW(0x104 + 0x10 * c, 0);
		VIGW(0x108 + 0x10 * c, src);
	}
	VIGW(0xa00 + 0x90, 0);                  /* PHASE_INIT Y_H, Y_V, UV_H, UV_V */
	VIGW(0xa00 + 0x94, 0);
	VIGW(0xa00 + 0x98, 0);
	VIGW(0xa00 + 0x9c, 0);
	VIGW(0xa00 + 0x10, stepX & 0xffffff);   /* PHASE_STEP Y_H, Y_V, UV_H, UV_V */
	VIGW(0xa00 + 0x14, stepY & 0xffffff);
	VIGW(0xa00 + 0x18, stepX & 0xffffff);
	VIGW(0xa00 + 0x1c, stepY & 0xffffff);
	VIGW(0xa00 + 0x20, 4u | 3u << 8 | 4u << 16 | 3u << 24);       /* PRELOAD: the default 4 across, 3 down */
	VIGW(0xa00 + 0x40, src);                /* SRC_SIZE_Y_RGB_A */
	VIGW(0xa00 + 0x44, src);                /* SRC_SIZE_UV */
	VIGW(0xa00 + 0x48, dst);                /* DST_SIZE */
	VIGW(0xa00 + 0x04, op);                 /* OP_MODE, last */
	IOLog("MiniIOMobileFramebuffer: fb_hw: VIG0 programmed: %ux%u -> %ux%u, steps %#x %#x, QSEED3 op %#x\n", width, height,
	    panelWidth, panelHeight, stepX, stepY, op);
}

/*
 * Before a frame is written into the buffer the panel showed until the last flip: waited for until the controller has
 * taken that flip (the flush bits read back 0 at the refresh that fetched the new address), at most 50 ms. The wait is
 * at the flip itself by default (see hwFlip); with fb_hw_async=1 at the next frame, CoreAnimation drawing meanwhile.
 */
static uint32_t gHwPending;
static TUNABLE(uint32_t, gHwAsync, "fb_hw_async", 0);

static void
hwWaitTaken(void)
{
	uint64_t began, until, waited;
	uint32_t flush = 0;

	if (!gHwPending) {
		return;
	}
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &until);
	until += began;
	while ((flush = *(volatile uint32_t *)(gHwCtl + 0x18)) != 0 && mach_absolute_time() < until) {
		IODelay(100);
	}
	waited = mach_absolute_time() - began;
	gHwPending = 0;
	gHwLatchWait += waited;
	gHwLatchMax = waited > gHwLatchMax ? waited : gHwLatchMax;
	gHwTimeouts += flush != 0;
	if ((gHwFlips & 1023) == 1 || flush != 0) {
		uint64_t avg, max, copy;

		absolutetime_to_nanoseconds(gHwLatchWait / gHwFlips, &avg);
		absolutetime_to_nanoseconds(gHwLatchMax, &max);
		absolutetime_to_nanoseconds(gHwCopy / gHwFlips, &copy);
		IOLog("MiniIOMobileFramebuffer: fb_hw: %llu flips, the copy %llu us, the wait for the controller %llu us (at most %llu), "
		    "%llu not taken in 50 ms%s\n", gHwFlips, copy / 1000, avg / 1000, max / 1000, gHwTimeouts,
		    flush != 0 ? " (THIS ONE)" : "");
	}
}

/* The buffer just written onto the panel: VIG0 at it and the CTL flushed; the controller takes it at its next refresh. */
static void
hwFlip(uint32_t width, uint32_t height, uint32_t panelWidth, uint32_t panelHeight)
{
	if (!gHwProgrammed) {
		hwProgram(width, height, panelWidth, panelHeight);
		gHwProgrammed = 1;
	}
	__asm__ volatile ("dsb sy" ::: "memory");      /* (the write-combined rows out before the address) */
	VIGW(0x14, (uint32_t) (gHwBack ? HW_BUFFER_PA1 : HW_BUFFER_PA0));
	*(volatile uint32_t *)(gHwCtl + 0x18) = 1u << 0 /* VIG0 */ | 1u << 17 /* CTL */;
	gHwPending = 1;
	gHwFlips++;
	gHwBack ^= 1;
	/*
	 * Waited for here, at the flip, by default (fb_hw_async=1: at the next frame instead). A frame that has left this
	 * driver is then one on the panel, and CoreAnimation's swaps are held back to the panel's refreshes: r271, waited here,
	 * 3% of the steps over 16.7 ms against 13% with the CPU scaling (r265); r272, waited at the next frame, could not be
	 * measured the same way (the frames' times are then the flips', not the refreshes').
	 */
	if (!gHwAsync) {
		hwWaitTaken();
	}
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	vm_offset_t     fBeamIntf;              /* fb_beam: the DSI interface's registers, once they were seen to count */
	uint32_t        fBeamLines;             /* ... lines of a scan, blanking included */
	uint64_t        fBeamPeriod;            /* ... how long a scan takes */
	uint64_t        fBeamScan;              /* ... when the scan that shows the frame being written begins */
	uint64_t        fBeamShare;             /* ... how long a thread's share of the rows takes to write */
	uint32_t        fStatScans, fStatScansMissed;   /* ... frames in time for the next scan, and those that were not */
	/* ... for each thread's share of the rows: how many, their time, begun after the scan reached them or not done before it left them, and done after it reached them */
	uint64_t        fStatShareTime[8];
	uint32_t        fStatShares[8], fStatShareTorn[8], fStatShareClose[8];
	uint32_t        fStatSettleReused;      /* ... frames not drawn again filtered: the GPU had their surface for the next frame */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint32_t        fStatSettled;           /* frames drawn again, filtered, once they stood still (fb_filter=2) */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */
	uint64_t        fRecCost, fRecCostMax;  /* what copying the recorded frames took (mach_absolute_time), and the worst */
	uint32_t        fRecCosted;             /* ... of how many frames: a line every 120 */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	void publishRefresh(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void settle(const uint8_t * swap, MiniIOSurface ** surfaces);
	bool fSettling;                 /* drawing the last frame again, filtered (fb_filter=2) */
	bool fFilterFrame;              /* this frame came slowly: filtered as it is (fb_filter=2) */
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	static void beamProbeMain(void * self, wait_result_t result);
	void beamProbe(void);
	uint64_t beamNextScan(uint64_t now);
	void beamWait(const struct RowJob * job, int32_t lastRow);
	void beamShareDone(const struct RowJob * job, unsigned share, int32_t firstRow, int32_t lastRow, uint64_t began);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

/*
 * The refresh the window server clocks its frames by: the property IOMFBDisplayRefresh, a dictionary of the shortest
 * interval between refreshes, the longest, and the step between them (QuartzCore, windowserver-apple-display.cpp:
 * displayMinRefreshInterval, displayRefreshStepMachTime; CoreMedia's FPSupport: displayMaxRefreshInterval,
 * displayRefreshStep). Without it CoreAnimation takes a built-in panel for a 60 Hz one whatever its timing says: the
 * mode it makes is "internal_panel (fixed)" at 60, UIScreen's maximumFramesPerSecond is 60, and frames came at 60 on
 * the 120 Hz panel (userland/src/fps_info.m, 30 October).
 * The intervals are in units of 2^-32 of a second (32.32 fixed point), not in mach_absolute_time's: given 1/120 s in
 * the phone's ticks (160000 of 19.2 MHz) CoreAnimation answered a refresh rate of 26844 a second, which is
 * 2^32 / 160000. Only the step whose name says MachTime is in ticks. This panel does not vary its rate, so all of
 * them are the one interval, fb_hz's.
 */
void
IOMobileFramebuffer::publishRefresh(void)
{
	OSDictionary * refresh = OSDictionary::withCapacity(4);
	uint32_t told = gCARefreshHz >= 1 && gCARefreshHz <= 480 ? gCARefreshHz : refreshHz();
	uint64_t interval = (1ULL << 32) / told, ticks;

	if (refresh == NULL) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC / told, &ticks);
	setNumber(refresh, "displayMinRefreshInterval", interval);
	setNumber(refresh, "displayMaxRefreshInterval", interval);
	setNumber(refresh, "displayRefreshStep", interval);
	setNumber(refresh, "displayRefreshStepMachTime", ticks);
	setProperty("IOMFBDisplayRefresh", refresh);
	refresh->release();
	IOLog("MiniIOMobileFramebuffer: IOMFBDisplayRefresh: %u Hz (the panel: %u), an interval of %llu (2^-32 s), %llu ticks\n",
	    told, refreshHz(), interval, ticks);
}

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	publishRefresh();
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	if (gBeam != 0 && gHw == 0 && kernel_thread_start(beamProbeMain, this, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "the panel's scan: looked at");
		thread_deallocate(thread);
	}
	if (gMeasure && kernel_thread_start(measurePanel, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (gMdpProbe && kernel_thread_start(mdpProbe, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (panel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (panel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	/*
	 * Boot-arg island=1 (the loader gives /product the iPhone 14 Pro's artwork-device-subtype, 2556): the screen is that
	 * phone's too, 1179 x 2556 (393 x 852 points), unless fbwidth/fbheight say otherwise. SpringBoard finds its screen type
	 * by its size in points; with the 14 Pro's subtype on the X's 375 x 812 it found none and laid the phone out as one
	 * with a home button -- no gestures, the old status bar, the island on the first row of icons (16 November). The 14
	 * Pro's 0.4613 is this panel's 0.4615 (1080 x 2340): scaled by 0.916 instead of 0.96, no more stretched.
	 */
	{
		uint32_t island = 0, given = 0;

		if (PE_parse_boot_argn("island", &island, sizeof(island)) && island == 1 &&
		    !PE_parse_boot_argn("fbwidth", &given, sizeof(given)) && !PE_parse_boot_argn("fbheight", &given, sizeof(given))) {
			fWidth = 1179;
			fHeight = 2556;
		}
	}
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		if (gFBSubtype == 0) {
			fWidth = kFBPointsWide * scale;
			fHeight = kFBPointsHigh * scale;
		} else {
			IORegistryEntry * product = IORegistryEntry::fromPath("/product", gIODTPlane);
			uint32_t subtype = gFBSubtype;

			if (product != NULL) {
				product->setProperty("artwork-device-subtype", &subtype, sizeof(subtype));
				product->release();
			}
		}
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	/* (as wide as the canvas too: with fb_hw a frame is drawn into a buffer of the canvas's width) */
	(void) hwSetup(fWidth, fHeight, fPanelWidth, fPanelHeight);    /* (the canvas and the panel known by now) */
	fColumn = IONewZero(uint32_t, fWidth > fPanelWidth ? fWidth : fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fWidth > fPanelWidth ? fWidth : fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/*
 * Are the first `rows` rows of a surface, `columns` 32-bit pixels of each, inside the memory it
 * has? Its stride, element height and size come from whoever made it, so they are checked here,
 * before anything reads the rows: in 64 bits, so that nothing wraps.
 */
static bool
surfaceHolds(const MiniIOSurface * s, uint64_t rows, uint64_t columns)
{
	if (rows == 0 || columns == 0) {
		return true;
	}
	return (uint64_t) s->info.offset + (rows - 1) * s->info.bytesPerRow + columns * 4 <= s->info.allocSize;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	/* a surface whose rows do not fit in its memory is not read */
	if (!surfaceHolds(s, fHeight, fWidth)) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	/* what the copy cost: it runs in the display's thread, between one frame and the next */
	now = mach_absolute_time() - now;
	fRecCost += now;
	fRecCostMax = now > fRecCostMax ? now : fRecCostMax;
	if (++fRecCosted == 120) {
		IOLog("MiniIOMobileFramebuffer: recording a frame took %llu us on average, %llu at worst (the last 120)\n",
		    micros(fRecCost) / 120, micros(fRecCostMax));
		fRecCost = fRecCostMax = 0;
		fRecCosted = 0;
	}
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
/* A layer's rectangle on the panel, in pixels, further out than this is not drawn (the panel is a few thousand wide). */
#define DST_LIMIT       (1 << 24)

struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         ry0, ry1;       /* the rows of the rectangle that are on the panel: all that is looked at, whatever its size */
	bool            filter, unscaled;
	uint64_t        scan;           /* fb_beam: when the scan that is to show the frame begins (0: write at once) */
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->ry1 - job->ry0;
		y0 = job->ry0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->ry0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->beamWait(job, y1);
		{
			uint64_t began = mach_absolute_time();

			fb->drawRows(job, y0, y1, blend);
			fb->beamShareDone(job, index + 1, y0, y1, began);
		}
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/* fb_beam: whether the interface's counters count as they should, and how many lines and how long a scan is. */
void
IOMobileFramebuffer::beamProbeMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->beamProbe();
}

void
IOMobileFramebuffer::beamProbe(void)
{
	vm_offset_t intf;
	uint32_t first, frames, line, lines = 0, smallest = ~0u;
	uint64_t began, ended, until;

	IOSleep(60000);
	intf = panelInterface();
	if (intf == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_beam: no display interface known for this SoC (fb_intf=): frames stay on their beat\n");
		return;
	}
	if (!(*(volatile uint32_t *)(intf + INTF_TIMING_ENGINE_EN) & 1)) {
		IOLog("MiniIOMobileFramebuffer: fb_beam: the interface's timing engine is off: frames stay on their beat\n");
		return;
	}
	/* a whole number of scans, from one frame's start to another's */
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (*(volatile uint32_t *)(intf + INTF_FRAME_COUNT) == first && mach_absolute_time() < until) {
	}
	began = mach_absolute_time();
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &until);
	until += began;
	while (mach_absolute_time() < until) {
		line = *(volatile uint32_t *)(intf + INTF_LINE_COUNT);
		lines = line > lines ? line : lines;
		smallest = line < smallest ? line : smallest;
	}
	IOSleep(450);
	frames = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (*(volatile uint32_t *)(intf + INTF_FRAME_COUNT) == frames && mach_absolute_time() < until) {
	}
	ended = mach_absolute_time();
	frames = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT) - first;
	if (frames == 0 || lines < fPanelHeight || lines > fPanelHeight * 2 || smallest > fPanelHeight / 4) {
		IOLog("MiniIOMobileFramebuffer: fb_beam: the interface's counters do not count as they should (%u frames, lines %u to %u): "
		    "frames stay on their beat\n", frames, smallest, lines);
		return;
	}
	fBeamLines = lines + 1;
	fBeamPeriod = (ended - began) / frames;
	absolutetime_to_nanoseconds(fBeamPeriod, &began);
	IOLog("MiniIOMobileFramebuffer: fb_beam: the panel's scan is %u lines, %llu us: frames are written behind it from now on\n",
	    fBeamLines, began / NSEC_PER_USEC);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	fBeamIntf = intf;
}

/* When the next scan the frame can be whole for begins: the first share has to be written before that scan reaches it. */
uint64_t
IOMobileFramebuffer::beamNextScan(uint64_t now)
{
	uint32_t line = *(volatile uint32_t *)(fBeamIntf + INTF_LINE_COUNT);
	uint64_t scan = now - fBeamPeriod * (line < fBeamLines ? line : fBeamLines - 1) / fBeamLines + fBeamPeriod;
	/*
	 * The first share is written from its top down about as fast as the scan reads it, so it may begin just before the
	 * scan does: by what it takes longer than the scan takes over the same rows, and a little. (Its whole time as a
	 * margin lost a frame in eight: each too late for one scan waits for the next, and all behind it with it.)
	 */
	uint64_t over = fThreads != 0 ? fBeamPeriod / fThreads : 0;
	uint64_t margin = (fBeamShare > over ? fBeamShare - over : 0) + fBeamPeriod / 16;
	bool missed = false;

	if (now + margin > scan) {
		scan += fBeamPeriod;
		missed = true;
	}
	/* (one frame a scan: the last frame's scan has its own) */
	while (fBeamScan != 0 && scan < fBeamScan + fBeamPeriod / 2) {
		scan += fBeamPeriod;
	}
	fStatScans++;
	fStatScansMissed += missed;
	return scan;
}

/* fb_beam: a share of the rows that ends at lastRow waits for the scan before the frame's to have passed it. */
void
IOMobileFramebuffer::beamWait(const struct RowJob * job, int32_t lastRow)
{
	if (job->scan != 0) {
		/* (the lines of the blanking are counted before the first row, as far as one can tell: taken as a margin either way) */
		uint64_t lines = (uint64_t) lastRow + (fBeamLines - fPanelHeight);
		uint64_t passed = job->scan - fBeamPeriod + fBeamPeriod * lines / fBeamLines;

		if (mach_absolute_time() < passed) {
			clock_delay_until(passed);
		}
	}
}

/* fb_beam: what a share of the rows did against the scan it is for (a frame torn at the share's edge is seen here, not in a screenshot). */
void
IOMobileFramebuffer::beamShareDone(const struct RowJob * job, unsigned share, int32_t firstRow, int32_t lastRow, uint64_t began)
{
	uint64_t now = mach_absolute_time();

	__atomic_fetch_add(&fStatShareTime[share], now - began, __ATOMIC_RELAXED);
	__atomic_fetch_add(&fStatShares[share], 1, __ATOMIC_RELAXED);
	if (job->scan != 0) {
		uint64_t blank = fBeamLines - fPanelHeight;
		uint64_t top = job->scan + fBeamPeriod * ((uint64_t) firstRow + blank) / fBeamLines;
		uint64_t bottom = job->scan + fBeamPeriod * ((uint64_t) lastRow + blank) / fBeamLines;

		if (began > top || now > bottom) {
			__atomic_fetch_add(&fStatShareTorn[share], 1, __ATOMIC_RELAXED);
		} else if (now > top) {
			__atomic_fetch_add(&fStatShareClose[share], 1, __ATOMIC_RELAXED);
		}
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter == 1 || (gFilter == 2 && (fSettling || fFilterFrame));
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rows and columns named must be in the surface's memory, whatever stride and size it was made with */
	if (!surfaceHolds(s, (uint32_t) (sy + sh), (uint32_t) (sx + sw))) {
		return;
	}
	/*
	 * the rectangle on the panel, worked out in 64 bits: the request's numbers are the caller's, and a
	 * rectangle of billions of rows is not drawn (nor looked at row by row)
	 */
	int64_t qx0 = (int64_t) dx * fPanelWidth / fWidth;
	int64_t qy0 = (int64_t) dy * fPanelHeight / fHeight;
	int64_t qx1 = ((int64_t) dx + dw) * fPanelWidth / fWidth;
	int64_t qy1 = ((int64_t) dy + dh) * fPanelHeight / fHeight;

	if (qx0 < -DST_LIMIT || qy0 < -DST_LIMIT || qx1 > DST_LIMIT || qy1 > DST_LIMIT) {
		return;
	}
	px0 = (int32_t) qx0;
	py0 = (int32_t) qy0;
	px1 = (int32_t) qx1;
	py1 = (int32_t) qy1;
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* nothing of it on the panel: nothing to draw (and the columns below are only worked out for ones that are) */
	if (px1 <= 0 || px0 >= (int32_t) fPanelWidth || py1 <= 0 || py0 >= (int32_t) fPanelHeight) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		bool allocated = fBlendRow != NULL;

		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			allocated = allocated && fHelperBlend[i] != NULL;
		}
		if (!allocated) {
			/* every array that did get made is given back at the size it was made with, so that no later growth frees one with the wrong size */
			if (fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
			for (unsigned i = 0; i + 1 < fThreads; i++) {
				if (fHelperBlend[i] != NULL) {
					IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
					fHelperBlend[i] = NULL;
				}
			}
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.ry0 = py0 < 0 ? 0 : py0;
	job.ry1 = py1 > (int32_t) fPanelHeight ? (int32_t) fPanelHeight : py1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.scan = fSettling ? 0 : fBeamScan;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, job.ry0, job.ry1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	{
		int32_t last = job.ry0 + (int32_t) ((int64_t) (job.ry1 - job.ry0) / fThreads);
		uint64_t wrote;

		beamWait(&job, last);
		wrote = mach_absolute_time();
		drawRows(&job, job.ry0, last, fBlendRow);
		beamShareDone(&job, 0, job.ry0, last, wrote);
		wrote = mach_absolute_time() - wrote;
		/* (on the average, lately: one preempted now and then is no reason to give up a scan every time) */
		fBeamShare += (int64_t) (wrote - fBeamShare) / 8;
	}
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS] = { NULL };
	bool unsettled = false;                 /* the frame on the panel was drawn unfiltered, and its surfaces are held */
	uint64_t lastFrame = 0, settleTime;
	unsigned fastFrames = 0;
	uint32_t shownDrawn[SWAP_LAYERS] = { 0 };       /* what the GPU had last been asked to draw into the surfaces on the panel */

	nanoseconds_to_absolutetime(SETTLE_MS * NSEC_PER_MSEC, &settleTime);

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			uint64_t deadline;

			if (!unsettled) {
				IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
				continue;
			}
			clock_interval_to_deadline(SETTLE_MS, NSEC_PER_MSEC, &deadline);
			if (IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && fSwapsWaiting == 0) {
				IOLockUnlock(fSwapLock);
				/* the frame's surfaces again, by their IDs: they were let go of as soon as the frame was on the panel */
				for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
					uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

					surfaces[layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
				}
				/*
				 * Not from a surface the GPU has been asked to draw into since: CoreAnimation has it back, and the
				 * next frame in it half drawn, tile by tile, is what the panel would get (bands of the picture with
				 * pieces missing, at the start of a gesture; the next frame comes in a moment anyway).
				 */
				{
					bool reused = false;

					for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
						reused = reused || (surfaces[layer] != NULL && surfaces[layer]->drawn != shownDrawn[layer]);
					}
					if (reused) {
						fStatSettleReused++;
					} else {
						settle(swap, surfaces);
					}
				}
				for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
					if (surfaces[layer] != NULL) {
						MiniIOSurfaceDrop(surfaces[layer]);
						surfaces[layer] = NULL;
					}
				}
				unsettled = false;
				IOLockLock(fSwapLock);
			}
		}
		IOLockUnlock(fSwapLock);
		/* the frame before is done with now, settled or not */
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
				surfaces[layer] = NULL;
			}
		}
		IOLockLock(fSwapLock);
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		{
			uint64_t now = mach_absolute_time();

			fastFrames = lastFrame != 0 && now - lastFrame <= settleTime ? fastFrames + 1 : 0;
			fFilterFrame = fastFrames < FAST_FRAMES;
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				shownDrawn[layer] = surfaces[layer] != NULL ? surfaces[layer]->drawn : 0;
			}
			show(swap, surfaces);
			lastFrame = mach_absolute_time();
		}
		unsettled = gFilter == 2 && !fFilterFrame && !gHwOn;     /* (fb_hw: the controller filters every frame) */
		/*
		 * The surfaces are let go of as soon as their pixels are in the panel's memory, not when the next frame comes:
		 * held, they were in use to CoreAnimation, which had no surface to draw the next frame into while that one
		 * waited for its beat (up to a refresh) -- its swaps came 9.6 ms apart at the median, not 8.3 (3 November,
		 * touch_lat=1). The filtered frame once they stop (fb_filter=2) takes them again by their IDs.
		 */
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
				surfaces[layer] = NULL;
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/*
 * The frame on the panel once more, filtered (fb_filter=2): the same layers from the same surfaces where show() put
 * them, nothing else (no turn to wait for, no swap to hand on, nothing counted). Its surfaces were held since.
 */
void
IOMobileFramebuffer::settle(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	int32_t src[4], dst[4];

	if (!fGraphics) {
		return;
	}
	fSettling = true;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		MiniIOSurface * s = surfaces[layer];

		if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) {
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		drawLayer(s, src, dst);
	}
	fSettling = false;
	fStatSettled++;
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	fBeamScan = fBeamIntf != 0 ? beamNextScan(began) : 0;
	if (fBeamIntf != 0) {
		/* (on the panel when that scan reads it: that is the beat now, kept by the panel) */
		fStatOnBeat++;
	} else if (gOnTheBeat && !gHwOn) {        /* (fb_hw: the controller's taking of a flip is the beat) */
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	uint8_t * panelBase = fBase;
	uint32_t panelWidth = fPanelWidth, panelHeight = fPanelHeight, panelRowBytes = fRowBytes;
	bool hw = gHwOn && fGraphics;

	if (gHwOn && !fGraphics) {
		/* (the first picture: the console is put away below, and from the next frame on the controller scales) */
		hw = false;
	}
	if (hw) {
		hwWaitTaken();                  /* (the buffer about to be written may still be the one on the panel) */
		began = mach_absolute_time();
		fBase = (uint8_t *) gHwVA[gHwBack];
		fPanelWidth = fWidth;
		fPanelHeight = fHeight;
		fRowBytes = gHwStride;
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
				gShownSurface = sid;
			}
		}
	}
	drawn = mach_absolute_time();
	if (hw) {
		gHwCopy += drawn - began;
		fBase = panelBase;
		fPanelWidth = panelWidth;
		fPanelHeight = panelHeight;
		fRowBytes = panelRowBytes;
		hwFlip(fWidth, fHeight, fPanelWidth, fPanelHeight);
		drawn = mach_absolute_time();
	}
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves; %u frames drawn again filtered; %u of %u too late for the next scan\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate, fStatSettled, fStatScansMissed, fStatScans);
		fStatScans = fStatScansMissed = 0;
		if (fBeamIntf != 0) {
			char shares[8 * 24] = "";
			size_t at = 0;

			for (unsigned share = 0; share < fThreads && share < 8; share++) {
				uint64_t took;

				absolutetime_to_nanoseconds(fStatShareTime[share], &took);
				at += (size_t) snprintf(shares + at, sizeof(shares) - at, " %llu/%u/%u", fStatShares[share] != 0 ? took / fStatShares[share] / NSEC_PER_USEC : 0,
				    fStatShareTorn[share], fStatShareClose[share]);
				fStatShareTime[share] = 0;
				fStatShares[share] = fStatShareTorn[share] = fStatShareClose[share] = 0;
			}
			IOLog("MiniIOMobileFramebuffer: shares of the rows, us each/torn/close:%s; %u not drawn again, their surface taken for the next frame\n", shares, fStatSettleReused);
			fStatSettleReused = 0;
		}
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = fStatSettled = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		armVSync();
		/*
		 * sent with the lock held: registerNotificationPort() swaps the port under this lock and
		 * only then lets go of the old one, so the port in `ref` cannot be released while it is sent to
		 * (the send does not wait: a full queue drops the message)
		 */
		IOReturn sent = sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
		if (gTouchLat) {
			/* "tl: y COUNT due US at US SENT": the refresh told to the window server, when it was due, when it went, and
			 * whether it went (0) or was dropped on a full queue -- for the window server's idle gaps (tools/frames.py) */
			hakios_tl("tl: y %llu due %llu at %llu %#x\n", fVSyncCount, micros(now), micros(mach_absolute_time()), sent);
		}
	}
	IOLockUnlock(fLock);
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		uint32_t metal = 0;

		accelerator->setName("sgx");
		/*
		 * Boot-arg metal=1: say where Metal's device is (docs/metal-plan.md, step 1). Metal reads these two
		 * properties, opens /System/Library/Extensions/<name>.bundle and takes the class (userland/src/HakiosMetal.m).
		 * Never by default: with them a program that asks for Metal gets a device, and CoreAnimation would choose
		 * Metal for the screen; until that device can draw, a screen drawn without it is the working one.
		 */
		if (PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0) {
			accelerator->setProperty("MetalPluginName", "HakiosMetal");
			accelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");
			IOLog("MiniIOMobileFramebuffer: metal=1: the accelerator names HakiosMetal as its Metal plug-in\n");
		}
		accelerator->registerService();
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
