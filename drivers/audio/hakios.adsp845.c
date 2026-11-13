/*
 * THE ONEPLUS 6T'S AUDIO DSP (SDM845, "ADSP", the Hexagon in LPASS), started, and spoken to: step 1 of
 * sound (11-13, lane B1). The scope is that of the reference phone's driver (drivers/audio/hakios.adsp.c,
 * left as it is, whose steps 1 to 10 this follows): the DSP up, SMP2P, GLINK, and QRTR on "IPCRTR", and the
 * list of the services the DSP offers. NOT here: the codec (WCD934x), SLIMbus, the speaker amplifier, APR
 * and the audio services, /dev/hakios-audio. Nothing is compiled in unless CONFIG_HAKIOS_ADSP845=y (it
 * stands in for CONFIG_HAKIOS_ADSP, which the 6T leaves out; the two cannot both be built), and nothing
 * is touched at run time unless the boot-arg adsp8=N (N >= 1) says so, or CONFIG_ADSP8 does, which makes
 * the same argument.
 *
 * What differs from the reference phone, each number from the 6T's own tree (the prebuilt device-tree.dtb of
 * the loader: qcom,lpass@17300000, qcom,smp2p-adsp@1799000c, qcom,glink-smem-native-xprt-adsp, reserved-memory
 * adsp_region@0x8c500000) and, where it has one, mainline's (ref-linux/sdm845.dtsi, drivers/remoteproc/
 * qcom_q6v5_pas.c "sdm845_adsp_resource_init"); this file is the same way as hakios.mpss845.c's, whose
 * lessons it takes (no IPCC; the shared memory at 0x86000000; the doorbells are bits of apss_shared):
 *   - TrustZone knows it as image 1 (dt: qcom,pas-id = 1; q6v5_pas: .pas_id = 1); its memory is
 *     0x8c500000, 0x1a00000 (26 MB), (dt: adsp_region@0x8c500000; ref-linux/sdm845.dtsi:846), which the
 *     image takes to its last byte (the header's last segment ends at 0x8df00000, where the Wi-Fi's
 *     wlan_msa_mem begins, sdm845.dtsi:851). The GPU's zap shader (0x8c415000, 0x2000, sdm845.dtsi:841) and
 *     the IPA's firmware (0x8c400000..0x8c415000) are below it: nothing overlaps. U-Boot r6 marks
 *     0x8c400000-0x97c00000 FAULT, so no speculative access reaches it
 *   - processor 2 in the shared memory (dt: qcom,remote-pid = <2>, sdm845.dtsi:1123); its SMP2P items
 *     443 (its) and 429 (ours) (sdm845.dtsi:1116 `qcom,smem = <443>, <429>`); its last words in item 423
 *     (dt: qcom,smem-id = 0x1a7; q6v5_pas .crash_reason_smem = 423); GLINK's three items 478..480 in our
 *     partition with it (the same numbers as the modem's)
 *   - NO IPCC: the doorbells are bits of apss_shared (0x17990000, +0xc): GLINK to the DSP bit 8 (dt:
 *     qcom,irq-mask = 0x100 of qcom,glink-smem-native-xprt-adsp; sdm845.dtsi:928 mboxes = <&apss_shared 8>),
 *     SMP2P bit 10 (dt: qcom,irq-bitmask = 0x400 of qcom,smp2p-adsp; sdm845.dtsi:1120 <&apss_shared 10>), the
 *     always-on processor bit 0 (the modem's: sdm845.dtsi aoss_qmp). Their interrupts (GIC SPI 156 GLINK, 158
 *     SMP2P, 162 the watchdog) are not used here: the shared memory is watched instead, as the modem's driver does
 *   - TrustZone is asked as for any PAS image (SCM 2/1 init_image, 2/2 mem_setup, 2/5 auth_and_reset, 2/6
 *     shutdown: ref-linux/qcom_scm.c; the GPU's zap shader runs on this phone through the same four, hakios.a630.c)
 *   - the always-on processor is told of it as "adsp" ("{class: image, res: load_state, name: adsp, val: on}":
 *     q6v5_pas .load_state = "adsp"; qcom_aoss.c qmp_send)
 *   - NO RAIL IS ASKED FOR by default: mainline's SDM845 ADSP has only the clock "xo" and no proxy power
 *     domain (sdm845_adsp_resource_init has no .proxy_pd_names; sdm845.dtsi:901 clocks = RPMH_CXO_CLK only).
 *     OnePlus' downstream tree names a proxy regulator "vdd_cx" (dt: qcom,proxy-reg-names = "vdd_cx") that
 *     the DSP's boot holds until it says "handover" (bit 2 of its SMP2P entry); adsp8_cx=1 asks RPMh for cx.lvl
 *     at level 384 while the DSP boots, as the modem's driver does for its CX, and gives it back at handover. It
 *     is the one RPMh request this file can make, and it is off unless asked for
 *   - the modem (hakios.mpss845.c) runs at the same time: the mailbox to the always-on processor and the lock
 *     for allocating in the shared memory are the two things both use, each behind the lock both take
 *     (hakios_aop_mailbox_lock, hakios_smem_alloc_lock, drivers/core/hakios.platform.c; and the hardware's
 *     own lock, the TCSR mutex 3 at 0x1f43000, taken under that); the doorbell register is written, never read,
 *     one bit at a time, so two drivers ringing it need no lock. This driver does not need the modem or the
 *     Wi-Fi up, and does not stop them
 *
 * Boot-arg adsp8=N performs steps 1 to N and stops (the DSP is shut down again unless adsp8_keep=1). As in
 * the reference driver a step says what it is about to touch before it touches it, and a step that does not
 * find what it needs ends the sequence. The image is in the system image's /fw/image/ (MODEM=1 SYSTEM=1
 * userland/build.sh copies fw-modem/image there whole: adsp.mdt and adsp.b00..b13 are in it), so the steps
 * wait for the root file system on a thread of their own, as the modem's do.
 *
 *   1  reads   the shared memory: its table of partitions, our partition with the DSP, its items, whether it
 *              has run since the phone was started, its last words; the black box of the run before
 *   2  reads   the firmware on the root: is it whole, does it fit the DSP's memory
 *   3  writes  the always-on processor's mailbox open, unless it is (the modem's start may have opened it)
 *   4  asks    TrustZone to take the image's header (image 1) and where the image will be; a DSP that a kernel
 *              before this one left running is asked to stop and shut down first
 *   5  writes  the image into the DSP's memory (0x8c500000), and reads it back
 *   6  writes  our SMP2P item (429), GLINK's descriptor (478) and our queue (479) into the shared memory
 *              (under the two locks), and our two marks of GLINK to zero
 *   7  asks    (adsp8_cx=1: RPMh for cx.lvl), writes "adsp845: on" to the always-on processor, asks TrustZone to
 *              authenticate and start the DSP, and watches for its first words (answering in our item what
 *              SMP2P has us answer; ringing SMP2P's doorbell, bit 10)
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents (doorbell
 *              bit 8); from here a thread of ours listens to the DSP
 *  10  talks   QRTR on that channel: hello both ways, and the services the DSP tells of, written down for
 *              adsp8_listen seconds (default 3)
 *  11  talks   APR (B2a), on the channel apr_audio_svc that the DSP opens and we answer from step 9 on (only when adsp8 is 11 or
 *              more: below that it is left unanswered as in B1): the core service (3) is asked whether the audio services are up
 *              (AVCS_CMD_ADSP_EVENT_GET_STATE, over and over for adsp8_apr seconds, default 15) and which services and versions it
 *              has; if they never say so, a diagnosis of what the DSP has told (the audio PD's notifier, the channels it opened).
 *              Only GLINK data and doorbell bit 8, as step 10. This file exports the reference phone's API of it:
 *              hakios_adsp_apr(), hakios_adsp_apr_events(), hakios_adsp_apr_events_second() (sys/hakios.audio.h)
 *  12  reads   (B2a) the speaker amplifier, a TFA9894 at 0x34 on QUP wrapper 0's engine 4: READS the clock controller's status of the
 *              engine's three clocks and the pins' control words (gpio58-61, 69, 89, 90); if the clocks are not all running it says so
 *              and stops, nothing of the engine touched; if they run, sets the engine up as an I2C master (its own registers) and READS
 *              the amplifier's revision register. No pin, clock, regulator or PMIC is written. Does not need the DSP: adsp8=812
 *              performs it alone (the 800 of adsp8=N: the DSP is left alone; adsp8=211 and 212 are the DSP kept up, APR, and this)
 *  13  --      (B2a) the playback sequence, a DESIGN: lane-b-notes/B2-test-plan.md chapter 6. The step refuses to run
 *
 * Writes to hardware registers: steps 3 (AOSS message RAM, apss_shared bit 0), 6 (the TCSR mutex), 7 and 9
 * (apss_shared bits 10 and 8; the AOSS again; with adsp8_cx=1 the RPMh sequencer), and TrustZone's calls
 * (4 and 7), and, in step 12 only if the clocks run, the I2C engine's own registers (0x890000). Nothing writes a clock, a pin or a PMIC. The kinds of write that are new to the 6T, besides
 * what the modem does already: TrustZone's PAS calls for image 1, "adsp" load state, and the apss_shared
 * bits 8 and 10.
 *
 * What is started is stopped: when the steps are over (unless adsp8_keep=1, and then when the run ends), and
 * on a panic's way back to the loader. The DSP is asked first (bit 0 of our entry "master-kernel", the reference
 * driver's way and Linux's qcom_q6v5_request_stop()), then TrustZone shuts it down, then the always-on
 * processor hears "adsp845: off". A DSP that was shut down without having said it stopped is not started again
 * unless adsp8_again=1.
 *
 * The boot line is nearly full (607 characters), so the one argument carries more: the hundreds of adsp8=N are switches,
 * 100 = adsp8_cx=1, 200 = adsp8_keep=1, 400 = adsp8_again=1 (added: adsp8=307 is step 7 with cx.lvl and kept up), 800 = the DSP is
 * left alone (adsp8=812 is the amplifier's probe alone). Named
 * arguments for what a config fragment can carry: adsp8_pause=MS (default 2200, what the loader needs to note the end of the log, after each announcement of the
 * last step), adsp8_wait=S (how long step 7 watches for "up", default 10), adsp8_listen=S, adsp8_keep=1,
 * adsp8_again=1, adsp8_cx=1, adsp8_bb=0 (the black box, below, off), adsp8_apr=S (how long step 11 asks the core service, default 15).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

#ifdef HAKIOS_ADSP
#error "CONFIG_HAKIOS_ADSP845 and CONFIG_HAKIOS_ADSP both define hakios_adsp_init(): build one"
#endif

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* What the modem's driver and this one both use (hakios.mpss845.c, drivers/core/hakios.platform.c). */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer, shared with the modem's driver */


static LCK_GRP_DECLARE(adsp_lock_group, "hakios-adsp845");

static void bb_say(const char *format, ...) __printflike(1, 2);

#define STEPS                   22
#define FIRST_INDEPENDENT_STEP  12      /* the steps from here on do not need the DSP (800 of adsp8=N leaves it alone) */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool flag_cx, flag_keep, flag_again;      /* the hundreds of adsp8=N: 100 asks RPMh for cx.lvl, 200 keeps the DSP up, 400 starts it again */
static bool flag_alone;                 /* 800: the DSP is left alone, only the steps that do not need it (12, 13) are performed */
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in hakios.wifi.c);
 * with adsp=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in hakios.wifi.c) ------------------------------------------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adsp845: step %u: %s\n", step, what);
	bb_say("step %u: %.120s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("adsp845: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	bb_say("%u: read %s %s (%#x)\n", step, b->name, name, b->phys + offset);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adsp845: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	bb_say("%u: write %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}


/* ---- a black box that outlives the CrashDump (as hakios.mpss845.c's) ------------------------------------ */

/*
 * The live log reaches the host every few seconds, so the last second before TrustZone's authentication takes the
 * phone to CrashDump is never seen. The lines of the steps go to RAM as well, in U-Boot r6's hakios-mdata@a0000000
 * (2M, no-map, mapped FAULT, kept by a warm reset; fajita u-boot/patches/sdm845-carveout.patch), at 0xa0110000, 32K:
 * the modem's black box is at 0xa0100000..0xa0110000 and the MBA's header is in the MBA's region, so the two do not
 * meet. The next run's step 1 says them before anything else, then clears them. adsp8_bb=0: not kept.
 */
#define BLACKBOX_PHYS           0xa0110000u
#define BLACKBOX_SIZE           0x8000u
#define BLACKBOX_MAGIC          0x38414b48u     /* "HKA8" */

static volatile char *blackbox;                 /* +0 magic, +4 bytes used, +8 run's epoch, +16 the lines */

static void
bb_say(const char *format, ...)
{
	char line[192];
	va_list arguments;
	uint32_t used, n;

	if (blackbox == NULL) {
		return;
	}
	va_start(arguments, format);
	n = (uint32_t)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (n >= sizeof(line)) {
		n = sizeof(line) - 1;
	}
	used = *(volatile uint32_t *)(blackbox + 4);
	if (used > BLACKBOX_SIZE - 16 || used + n > BLACKBOX_SIZE - 16) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		blackbox[16 + used + i] = line[i];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(blackbox + 4) = used + n;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Step 1: what the run before left, said; then a fresh box for this one. */
static void
bb_open(void)
{
	uint32_t magic, used, epoch = 0, on = 1;

	if (blackbox != NULL) {
		return;
	}
	PE_parse_boot_argn("adsp8_bb", &on, sizeof(on));
	if (on == 0) {
		return;
	}
	blackbox = (volatile char *)ml_io_map(BLACKBOX_PHYS, BLACKBOX_SIZE);
	magic = *(volatile uint32_t *)blackbox;
	used = *(volatile uint32_t *)(blackbox + 4);
	if (magic == BLACKBOX_MAGIC && used <= BLACKBOX_SIZE - 16) {
		char line[200];
		uint32_t l = 0;

		tell("adsp845: THE BLACK BOX OF THE RUN BEFORE (%u bytes, its epoch %u):\n", used, *(volatile uint32_t *)(blackbox + 8));
		for (uint32_t i = 0; i < used; i++) {
			char c = blackbox[16 + i];

			if (c == '\n' || l == sizeof(line) - 1) {
				line[l] = 0;
				printf("adsp845: bb<  %s\n", line);
				l = 0;
				continue;
			}
			line[l++] = (c >= 32 && c < 127) ? c : '.';
		}
		if (l != 0) {
			line[l] = 0;
			printf("adsp845: bb<  %s\n", line);
		}
	} else {
		printf("adsp845: the black box at %#x: nothing of a run before (magic %#x)\n", BLACKBOX_PHYS, magic);
	}
	PE_parse_boot_argn("epoch", &epoch, sizeof(epoch));
	*(volatile uint32_t *)(blackbox + 4) = 0;
	*(volatile uint32_t *)(blackbox + 8) = epoch;
	*(volatile uint32_t *)blackbox = BLACKBOX_MAGIC;
	__asm__ volatile ("dsb sy" ::: "memory");
}


/* ---- the shared memory (hakios.mpss845.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               0x86000000UL   /* 6T: memory@86000000 qcom,smem (ref-linux/sdm845.dtsi:800; the 6T's dt: qcom,smem@86000000) */
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_ADSP               2               /* dt: qcom,remote-pid = <2> of qcom,smp2p-adsp (sdm845.dtsi:1123) and glink-edge "lpass" (:927) */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (ref-linux/sdm845.dtsi:1116 smp2p-lpass: qcom,smem = <443>, <429>: the item the DSP writes first,
 * then ours -- taken the other way round, the DSP made our item its own and never said "up" (the reference phone's
 * lesson) --, and qcom,smem-id = 0x1a7 = 423 of qcom,lpass@17300000 in the 6T's own dt; q6v5_pas
 * sdm845_adsp_resource_init .crash_reason_smem = 423).
 */
#define ITEM_SMP2P_OURS         429
#define ITEM_SMP2P_THEIRS       443
#define ITEM_CRASH_REASON       423

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		/*
		 * The DSP can write this header: padding larger than the item would make the size below wrap to about 4 GB,
		 * and an item that ends beyond its partition would send the readers out of the shared memory. Stop at it.
		 */
		if (entry.padding_data > entry.size ||
		    (uint64_t)at + sizeof(entry) + entry.padding_header + entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		/* (padding larger than the item would make the size below wrap to about 4 GB) */
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at ||
		    entry.padding_data > entry.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp845: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("adsp845: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("adsp845: %u:   items (bytes):%s\n", step, line);
	}
}


/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (sdm845.dtsi:909 adsp_pas interrupt-names "fatal", "ready", "handover", "stop-ack" are bits 0..3 of the entry "slave-kernel" of smp2p-lpass). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (sdm845.dtsi:1125 the child "master-kernel" of smp2p-lpass; adsp_pas qcom,smem-states = <&adsp_smp2p_out 0> "stop"). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("adsp845: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("adsp845: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("adsp845: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("adsp845: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}


/* ---- the doorbells, and the mailbox to the always-on processor (hakios.mpss845.c) ----------------------- */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

/* The AOSS's message RAM (ref-linux/sdm845.dtsi aoss_qmp reg 0x0c300000; the modem's driver maps 0x1000 of it as this does). */
static struct block aoss = { "mailbox", 0x0c300000, 0x1000 };

/*
 * The doorbells are bits of apss_shared's IPC register (ref-linux/sdm845.dtsi:5258 mailbox@17990000,
 * "qcom,sdm845-apss-shared"; qcom-apcs-ipc-mailbox.c: a write of BIT(n) at +12 rings doorbell n). Written, never
 * read, so the modem's driver ringing its own bits at the same time is no hazard.
 */
static struct block apss = { "apss_shared", 0x17990000, 0x1000 };
#define APSS_IPC                0x0c
#define APSS_AOP                0               /* sdm845.dtsi aoss_qmp: mboxes = <&apss_shared 0> */
#define APSS_GLINK              8               /* sdm845.dtsi:928 glink-edge "lpass": mboxes = <&apss_shared 8>; the 6T's dt: qcom,irq-mask 0x100 */
#define APSS_SMP2P              10              /* sdm845.dtsi:1120 smp2p-lpass: mboxes = <&apss_shared 10>; the 6T's dt: qcom,irq-bitmask 0x400 */

static void
apss_ring(unsigned bit)
{
	need(&apss);
	bb_say("%u: ring apss_shared bit %u\n", step, bit);
	wr(apss.base + APSS_IPC, 1u << bit);
}

/* What the DSP's rings amount to is not counted: they are interrupts of the GIC here, and the shared memory is watched instead. */
static struct {
	uint64_t since;
} doorbell;

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */


static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	apss_ring(APSS_AOP);
}

/* Linux's qmp_open(), as hakios.wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("adsp845: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	/* (box and size come from the always-on processor: "box + size" could wrap in 32 bits and pass, so it is compared as a difference) */
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box >= aoss.size || size > aoss.size - box || (box & 3)) {
		printf("adsp845: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("adsp845: %u: the mailbox is open already (the modem's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("adsp845: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("adsp845: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("adsp845: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("adsp845: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the modem's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hakios_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("adsp845: %u: about to tell the always-on processor \"%s\"\n", step, text);
	bb_say("%u: to the always-on processor: %.60s\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("adsp845: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("adsp845: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("adsp845: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}


/* ---- RPMh: the DSP's rail, asked for only with adsp8_cx=1 ------------------------------------------------ */

/*
 * Mainline asks for NO rail for the SDM845's ADSP (q6v5_pas sdm845_adsp_resource_init: no .proxy_pd_names; only the
 * clock "xo"). OnePlus' downstream tree has a proxy regulator "vdd_cx" (the 6T's dt: qcom,lpass@17300000
 * qcom,proxy-reg-names = "vdd_cx") that the DSP's boot holds until it says "handover" (bit 2). On the reference phone,
 * without such a vote the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words. So
 * adsp8_cx=1 asks RPMh for cx.lvl at level 384 while the DSP boots, as the modem's driver does for its CX (hakios.mpss845.c
 * rails()), and gives it back at handover or when the DSP is shut down. Off by default: a new RPMh vote.
 *
 * A request goes to RPMh through the application processor's sequencer (ref-linux/sdm845.dtsi apps_rsc rsc@179c0000;
 * the modem's driver uses DRV 2 at 0x179e0000 and so does this; qcom,tcs-offset 0xd00), as Linux's rpmh-rsc.c writes one:
 * the command's message id, address and value into a TCS that is idle, the command enabled and marked to be waited for,
 * the TCS put in "active" mode and triggered, and its bit in the status of completions looked for. A rail asked for by
 * level is asked by position in the list of its levels that the boot loader's command database keeps (cmd-db.c).
 */
#define CMD_DB_PHYS             0x85fe0000UL   /* 6T: aop-cmd-db-mem@85fe0000 (ref-linux/sdm845.dtsi:794) */
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x179e0000, 0x10000 };     /* 6T: apps_rsc DRV 2, as hakios.mpss845.c */
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("adsp845: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("adsp845: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("adsp845: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}


/* The one rail at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. Only with adsp8_cx=1. */
static bool
rails(uint32_t level)
{
	uint32_t address = 0, value = 0, done = 0;
	int position;
	uint64_t began;

	position = level == 0 ? 0 : rail_position("cx.lvl", level, &address);
	if (level == 0) {
		rail_position("cx.lvl", 0, &address);
	}
	if (position < 0 || address == 0) {
		printf("adsp845: %u: no position for cx.lvl at level %u: the rail is not asked\n", step, level);
		return false;
	}
	value = (uint32_t)position;
	need(&rsc);
	/* (on a panic's way back to the loader no lock is taken: the other processors are stopped, and one of them may hold this one for good) */
	if (!leaving) {
		lck_mtx_lock(&hakios_rsc_lock);
	}
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("adsp845: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rail is not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		if (!leaving) {
			lck_mtx_unlock(&hakios_rsc_lock);
		}
		return false;
	}
	printf("adsp845: %u: about to ask RPMh, through TCS %u, for cx.lvl (%#x) = %u (level %u%s)\n", step, RPMH_TCS, address, value,
	    level, level == 0 ? ": given back" : "");
	bb_say("%u: RPMh cx.lvl %#x = %u (level %u)\n", step, address, value, level);
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for cx.lvl at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0), MSGID_WRITE_WAITED);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 4, address);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 8, value);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 1);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 1);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("adsp845: %u: RPMh %s after %llu us; the command's status %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	if (!leaving) {
		lck_mtx_unlock(&hakios_rsc_lock);
	}
	return done != 0;
}


/* ---- step 1: the shared memory --------------------------------------------------------------------- */

/* Every partition the shared memory's table has: which pairs of processors there are, and where. */
static void
show_toc(void)
{
	struct smem_table table;
	struct smem_table_entry entry;
	char line[320];
	int at = 0;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		printf("adsp845: %u: the shared memory has no table of partitions this knows\n", step);
		return;
	}
	line[0] = 0;
	for (uint32_t i = 0; i < table.count && at < (int)sizeof(line) - 48; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		at += snprintf(line + at, sizeof(line) - (size_t)at, " (%u,%u)@%#x", entry.host0, entry.host1, entry.offset);
	}
	printf("adsp845: %u: the table has %u entries; the partitions (hosts)@offset:%s\n", step, table.count, line);
}

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	bb_open();
	announce("read the shared memory's table of partitions, our partition with the audio DSP, and its items");
	printf("adsp845: %u: the shared memory at %#lx\n", step, SMEM_PHYS);
	show_toc();
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("adsp845: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("adsp845: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "not there (made in step 6)",
	    smem_item(479, &size) ? "there" : "not there (made in step 6)", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}


/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define ADSP_MEMORY             0x8c500000u     /* 6T dt: reserved-memory adsp_region@0x8c500000, memory-region of qcom,lpass@17300000; ref-linux/sdm845.dtsi:846 */
#define ADSP_MEMORY_SIZE        0x01a00000u     /* 26 MB (the same two places) */
#define PAS_ADSP                1               /* 6T dt: qcom,pas-id = <1>; q6v5_pas sdm845_adsp_resource_init .pas_id = 1 */
#define HEADER_MAX              0x10000         /* the header file: 7452 bytes here (532 of ELF header and 6920 of hashes) */


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-modem/image/ when it makes that image with MODEM=1 (adsp.mdt and adsp.b00..b13 are among them). It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only adsp8=N, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	/* (fw-modem/image is copied whole to /fw/image by MODEM=1 SYSTEM=1 userland/build.sh; /fw is tried too, as the reference driver does) */
	snprintf(path, sizeof(path), "/fw/image/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		snprintf(path, sizeof(path), "/fw/%s", name);
		if (vnode_lookup(path, 0, &vnode, context) != 0) {
			return NULL;
		}
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > ADSP_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("adsp845: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* The firmware is on the root (the small image is too small for it: 21 MB). */
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	const uint8_t *data = root_file(name, size);

	if (data != NULL) {
		from_root = true;
		return data;
	}
	printf("adsp845: %u: no /fw/image/%s on the root: the phone's /vendor/firmware_mnt/image/ is in fw-modem/image/ and goes there with "
	    "MODEM=1 SYSTEM=1 userland/build.sh\n", step, name);
	return NULL;
}

#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;
	uint8_t first[4] = { 0 };

	announce("find the audio DSP's image on the root (/fw/image), and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("adsp.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	/* (a file of 1 to 4 bytes is shorter than what is compared and printed: both look only at what is there) */
	memcpy(first, header, size < sizeof(first) ? size : sizeof(first));
	if (size < 5 || memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size ||
	    size > HEADER_MAX) {
		printf("adsp845: %u: adsp.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, first[0],
		    first[1], first[2], first[3], count, size);
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < ADSP_MEMORY || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)ADSP_MEMORY + ADSP_MEMORY_SIZE) {
			printf("adsp845: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - ADSP_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "adsp.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("adsp845: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				/* (the file is given back with the size it was allocated with, not the header's: kfree_data() needs that) */
				image.segment[image.segments].file_size = (uint32_t)file_size;
				image.segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != ADSP_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("adsp845: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - ADSP_MEMORY;
	printf("adsp845: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, "from /fw on the root", size, hashes,
	    image.segments, files, image.size, ADSP_MEMORY_SIZE, ADSP_MEMORY);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}


/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&hakios_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return open;
}


/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

/* The four PAS calls and their arguments: ref-linux/qcom_scm.c:575 init_image (2 arguments: id, a buffer read and written), :664 mem_setup
 * (id, address, size), :708 auth_and_reset (id), :744 shutdown (id); service 2 (SVC_PIL). The AOSS message: ref-linux/qcom_q6v5.c:31. */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* (adsp8_cx=1) our request for cx.lvl stands (until the DSP asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("adsp845: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adsp845: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adsp845: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	/* (step_our_item() writes the whole struct there without a bounds check: it has to lie inside the SMEM window) */
	return at != 0 && size >= sizeof(struct smp2p) && (uint64_t)at + sizeof(struct smp2p) <= SMEM_SIZE ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("adsp845: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("adsp845: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		apss_ring(APSS_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("adsp845: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("adsp845: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("adsp845: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("adsp845: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("adsp845: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell (apss_shared bit 10)\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	apss_ring(APSS_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("adsp845: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	apss_ring(APSS_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);
static void meta_release(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("adsp845: %u: %s: the audio DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the audio DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp845: %u: TRUSTZONE DOES NOT SHUT THE AUDIO DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("adsp845: %u: THE AUDIO DSP IS SHUT DOWN\n", step);
	meta_release();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}


/*
 * The header file is 7452 bytes here, more than a page, and TrustZone reads it as one buffer by its physical address
 * (qcom_scm_pas_init_image: a coherent DMA buffer): so it is made in memory that is one physical piece. Linux gives it
 * back after the DSP has been started or shut down (qcom_scm_pas_metadata_release()); so does this.
 */
static vm_offset_t meta_page;
static vm_size_t meta_size;

static void
meta_release(void)
{
	if (meta_page != 0 && !leaving) {
		kmem_free(kernel_map, meta_page, meta_size);
	}
	meta_page = 0;
	meta_size = 0;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	vm_size_t page_size;
	uint64_t answer = ~0ull;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an audio DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !(flag_again || option("adsp8_again"))) {
			printf("adsp845: %u: THE AUDIO DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP of the reference phone a start after that reset the phone (adsp8_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("adsp845: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in memory of ours that is one physical piece");
	meta_release();
	page_size = round_page(image.header_size);
	if (kmem_alloc_contig(kernel_map, &page, page_size, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adsp845: %u: no memory\n", step);
		return false;
	}
	meta_page = page;
	meta_size = page_size;
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + page_size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the audio DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_ADSP, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp845: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		meta_release();
		return false;
	}
	printf("adsp845: %u: TRUSTZONE ACCEPTS THE AUDIO DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, ADSP_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp845: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		/* (the header stays with TrustZone until the image is shut down: PAS_SHUTDOWN of the same id lets it go) */
		scm("to let the image go", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
		meta_release();
		return false;
	}
	printf("adsp845: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, ADSP_MEMORY);
	return true;
}


/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the audio DSP's memory (0x8c500000, 26 MB: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(ADSP_MEMORY, ADSP_MEMORY_SIZE, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, ADSP_MEMORY);
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;

		/* (the header has a segment of 0x54 bytes: it is written to a multiple of eight, zeroes beyond its end, inside its page) */
		if (image.segment[s].at & 7) {
			printf("adsp845: %u: a segment that does not begin at a multiple of eight bytes: not written\n", step);
			ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
			return false;
		}
		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {      /* (memory_size rounded up by the loop: the last word) */
			to[i / 8] = segment_word(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;

		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
	}
	printf("adsp845: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}


/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u              /* the application processors' id as owner of the TCSR mutex; sdm845.dtsi:800 smem hwlocks = <&tcsr_mutex 3> */

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (hakios.mpss845.c: our_item_make()). */
static bool
our_item_make(uint32_t partition, uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);         /* (the modem's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("adsp845: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("adsp845: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("adsp845: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("adsp845: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("adsp845: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - bytes);
	for (uint32_t z = 0; z < item_size; z += 4) {
		smem_poke(partition + at + (uint32_t)sizeof(entry) + z, 0);
	}
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("adsp845: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it and GLINK's two (478, 479) if there are none, and fill ours in");
	partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp845: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	{
		/*
		 * GLINK's descriptor (478) and our queue (479, 16K) are ours to make before the DSP starts, as Linux's
		 * qcom_glink_smem_register() makes them at its probe; the DSP makes only its own queue (480). The modem's driver found
		 * the same on this phone (hakios.mpss845.c step 4, run 24).
		 */
		uint32_t isize = 0;

		if (smem_item(478u, &isize) == 0 && our_item_make(partition, 478u, 32)) {
			printf("adsp845: %u: GLINK's descriptor (item %u) made, 32 bytes, zeroed\n", step, 478u);
		}
		if (smem_item(479u, &isize) == 0 && our_item_make(partition, 479u, 0x4000)) {
			printf("adsp845: %u: our GLINK queue (item %u) made, 0x4000 bytes, zeroed\n", step, 479u);
		}
		printf("adsp845: %u: GLINK's descriptor (478): %s, our queue (479): %s\n", step, smem_item(478u, &isize) ? "there" : "NOT THERE",
		    smem_item(479u, &isize) ? "there" : "NOT THERE");
	}
	if (our_item != 0) {
		printf("adsp845: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition, ITEM_SMP2P_OURS, (uint32_t)sizeof(struct smp2p))) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("adsp845: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("adsp845: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	apss_ring(APSS_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("adsp845: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	glink_reset();
	return true;
}


/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * (On the reference phone: the DSP's SMP2P item appears 17 ms after TrustZone returns; at 41 ms "slave-kernel" says 6,
 * up and asking for its own power; asked to stop, it acknowledges within 25 ms. Nothing of that has been seen on the
 * 6T yet: this slice has not run on it.)
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("adsp845: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("adsp8_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("(adsp8_cx=1: ask RPMh for cx.lvl,) tell the always-on processor that the image is loaded, and ask TrustZone to "
	    "authenticate the image and let the DSP out of reset");
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (flag_cx || option("adsp8_cx")) {
		if (!rails(384)) {
			lck_mtx_unlock(&dsp_lock);
			return false;
		}
		rails_asked = true;
	}
	if (!mailbox_send("{class: image, res: load_state, name: adsp, val: on}")) {
		if (rails_asked) {
			rails(0);
			rails_asked = false;
		}
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the audio DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("adsp845: %u: TRUSTZONE DOES NOT START THE AUDIO DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		/* (Linux, after a start that fails: the image is shut down, which lets TrustZone go of it) */
		scm("to let the image go after a start that failed", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
		meta_release();
		mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
		if (rails_asked) {
			rails(0);
			rails_asked = false;
		}
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	meta_release();                         /* (Linux: qcom_scm_pas_metadata_release() once the image is authenticated) */
	lck_mtx_unlock(&dsp_lock);
	printf("adsp845: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE AUDIO DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("adsp845: %u: THE AUDIO DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("adsp845: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("adsp845: %u: THE AUDIO DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("adsp845: %u: the watch is over after %llu ms\n", step, (now_us() - started) / 1000);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (429) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("adsp845: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("adsp845: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("adsp845: %u: %s\n", step, spoke ? "THE AUDIO DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE AUDIO DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}


/* ---- GLINK (hakios.mpss845.c; items 478 to 480 of our partition with the audio DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channel we answer when the DSP opens it, with our number for it and the intents we give it when it is open
 * both ways (how many buffers of which size the DSP may send into): IPCRTR, which carries QRTR (ref-linux/sdm845.dtsi has
 * no qrtr child for lpass; the reference phone's dt qcom,intents of qcom,adsp_qrtr). The DSP's others (apr_audio_svc and
 * the rest) are left unanswered: the audio services are for the steps after this one.
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNELS };

struct glink_channel {
	const char *name;
	bool wanted;                            /* answered when the DSP opens it (else: left unanswered, as in B1) */
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .wanted = true, .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		/*
		 * Step 11 (adsp8=11 and more): APR, the audio services' protocol. dt: ref-linux/sdm845.dtsi:930-936, apr-v2 with
		 * qcom,glink-channels = "apr_audio_svc" and qcom,intents = <512 20>, 20 buffers of 512 bytes (Linux's
		 * qcom_glink_native.c gives them to the channel when it is open, as glink_pump() does for the others). Not
		 * answered unless step 11 is to be performed (hakios_adsp_init() sets "wanted"): the steps up to 10 stay as in B1.
		 */
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 512, 20 }, { 0, 0 }, { 0, 0 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	/* (the descriptor and both queues are read and written without further checks: all three must lie inside the SMEM window) */
	if (glink.descriptor == 0 || size != 32 || (uint64_t)glink.descriptor + size > SMEM_SIZE) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7) &&
	       (uint64_t)glink.ours + glink.ours_size <= SMEM_SIZE && (uint64_t)glink.theirs + glink.theirs_size <= SMEM_SIZE;
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	/* (and inside the SMEM window: the words below are read and written without a bounds check) */
	if (descriptor == 0 || size != 32 || (uint64_t)descriptor + size > SMEM_SIZE) {
		printf("adsp845: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("adsp845: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("adsp845: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	apss_ring(APSS_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("adsp845: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		/* (a buffer of another size from an earlier run is given back first: replacing the pointer would leak it) */
		if (channel->our_intents[i].data != NULL) {
			kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
			channel->our_intents[i].data = NULL;
		}
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length;
		/*
		 * The length is worked out in 64 bits: the DSP writes the fields it comes from, and in 32 bits a value such as
		 * 0xfffffff8 wrapped it to 0, so that the message was never taken out and this loop never ended (with glink_lock held).
		 */
		uint64_t wide = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			wide += message.param2;
		} else if (message.command == GLINK_INTENT) {
			wide += 8ull * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			wide = 16ull + words[0];
		}
		wide = (wide + 7) & ~7ull;
		if (wide > glink.theirs_size - 8) {
			printf("adsp845: glink: A MESSAGE OF %llu BYTES (%s %u %#x): not believed, and nothing more is read\n", wide,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		length = (uint32_t)wide;
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("adsp845: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (glink.channel[c].wanted && strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("adsp845: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("adsp845: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("adsp845: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			apss_ring(APSS_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("adsp845: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("adsp845: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("adsp845: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}


/* ---- QRTR: hello, and the services the DSP tells of (hakios.mpss845.c, without ports of our own) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;
/* QMI over QRTR (the microphone's SLIMbus service, 0x301): one question at a time, its answer kept here by qrtr_input() */
#define QMI_OUR_PORT            0x5301u
static uint8_t qmi_reply[256];
static volatile uint32_t qmi_reply_length;
static volatile bool qmi_replied;
static uint32_t qrtr_looked_for[8];     /* the services (ids) the DSP looked for, each once: the diagnosis of step 11 says them */

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("adsp845: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		printf("adsp845: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("adsp845: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		for (unsigned i = 0; i < sizeof(qrtr_looked_for) / sizeof(qrtr_looked_for[0]); i++) {
			if (qrtr_looked_for[i] == control->words[0]) {
				break;
			}
			if (qrtr_looked_for[i] == 0) {
				qrtr_looked_for[i] = control->words[0];
				break;
			}
		}
		if (qrtr_lookups++ < 16) {
			printf("adsp845: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("adsp845: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("adsp845: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	/* (the datagram's size is the DSP's word: "at + size" could wrap in 32 bits and pass, so it is compared as a difference) */
	if (header.size == 0 || at > length || header.size > length - at) {
		printf("adsp845: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	if (header.to_port == QMI_OUR_PORT) {
		uint32_t n = header.size < sizeof(qmi_reply) ? header.size : (uint32_t)sizeof(qmi_reply);

		memcpy(qmi_reply, data + at, n);
		qmi_reply_length = n;
		OSMemoryBarrier();
		qmi_replied = true;
		return;
	}
	printf("adsp845: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}


/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

static void apr_input(const uint8_t *data, uint32_t length);

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("adsp845: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_APR) {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("adsp845: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams\n",
		    link_runs ? "THE THREAD DOES NOT END" : "down", glink.said, glink.heard, qrtr_out, qrtr_in);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
		/*
		 * The buffers we offered the DSP are given back too, so that they do not stay allocated for good: only
		 * glink_pump() uses them, under this lock, and it does nothing while glink.up is false.
		 */
		for (unsigned i = 0; i < sizeof(channel->our_intents) / sizeof(channel->our_intents[0]); i++) {
			if (channel->our_intents[i].data != NULL) {
				kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
				channel->our_intents[i].data = NULL;
			}
			channel->our_intents[i].size = channel->our_intents[i].fill = 0;
		}
		channel->our_intent_count = 0;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}


/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
	if (!glink_find()) {
		printf("adsp845: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("adsp845: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("adsp845: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the audio DSP: our version, then whatever its messages call for (doorbell: apss_shared bit 8); a "
	    "thread of ours listens from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	bzero(qrtr_looked_for, sizeof(qrtr_looked_for));
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("adsp845: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("adsp845: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN",
	    glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("adsp8_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("adsp845: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}


/* ---- APR: the audio services' protocol, on the channel apr_audio_svc (step 11) ------------------------ */

/*
 * What Linux's audio drivers say to the DSP's audio services (Linux v6.12: drivers/soc/qcom/apr.c, include/linux/soc/qcom/apr.h,
 * include/dt-bindings/soc/qcom,apr.h, sound/soc/qcom/qdsp6/q6core.c; this is written again from what they do, and from the
 * reference phone's hakios.adsp.c, whose API it keeps: hakios.audio.h): a packet is a header of five words -- a field of
 * version (bits 0-3), header size in words (4-7) and message type (8-9); the packet's size; who it is from and to (a service, a
 * domain, a port each: the DSP is domain 4, we 5); a token that the answer carries back; an opcode -- and what the opcode takes.
 * apr.c's apr_send_pkt() fills the source and destination (service and domain) of what a driver sends; the ports and the token of
 * the core service's questions are 0 (q6core.c). The DSP's own core service (3) tells whether the audio services are up and which
 * services and versions it has.
 *
 * What the reference phone's driver did not check, and this does (docs/driver-review/sound.md S-13: a packet whose own size is below
 * its header's made "size - header" wrap to about 4 GB, and the copies after it read from beyond the packet): a packet is believed
 * only when its header is whole, its size is neither below the header's nor above what arrived (Linux's apr_do_rx_callback() asks
 * for equal; here a few bytes of padding after it would do), its version, message type, domains and services are in range.
 */
#define APR_DOMAIN_ADSP                 4       /* qcom,apr.h */
#define APR_DOMAIN_APPS                 5
#define APR_DOMAIN_MAX                  6
#define APR_SVC_ADSP_CORE               3       /* sdm845.dtsi:938 service@3 "qcom,q6core" */
#define APR_SVC_AFE                     4       /* :944 q6afe */
#define APR_SVC_ASM                     7       /* :956 q6asm (iommus 0x1821 at :965) */
#define APR_SVC_ADM                     8       /* :969 q6adm */
#define APR_SVC_MAX                     0x17
#define APR_MSG_TYPE_SEQ_CMD            2       /* apr.h */
#define APR_PKT_VER                     0
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)(((type) & 3) << 8 | (((bytes) / 4) & 0xf) << 4 | APR_PKT_VER))
#define APR_BASIC_RSP_RESULT            0x000110e8u     /* apr.h: payload { opcode, status } */
#define ADSP_EUNSUPPORTED               3u              /* q6dsp-errno.h */
#define AVCS_GET_VERSIONS               0x00012905u     /* q6core.c */
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du
#define AVCS_CMD_GET_FWK_VERSION        0x0001292cu     /* (q6core.c writes 0x001292c: the same number) */
#define AVCS_CMDRSP_GET_FWK_VERSION     0x0001292du
#define APR_SEND_MAX                    256             /* the most payload a command of ours carries */
#define APR_WORDS                       128             /* the most payload of an answer kept here (512 bytes, one of our intents) */

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};
_Static_assert(sizeof(struct apr_header) == 20, "APR's header is five words");

struct apr_reply {
	uint32_t opcode, token, length;         /* length: of the payload, in bytes (the words kept are at most APR_WORDS) */
	uint8_t from_service;
	uint32_t words[APR_WORDS];
};

/* The answer a question waits for: written by the link's thread, read by the asker, both under apr_answer_lock. */
static LCK_MTX_DECLARE(apr_answer_lock, &adsp_lock_group);
static LCK_MTX_DECLARE(apr_lock, &adsp_lock_group);     /* one question at a time */
static struct apr_reply apr_answer;
static bool apr_answer_there;
static bool apr_waiting, apr_waiting_any;               /* a question waits; any token from its service will do (the core's) */
static uint32_t apr_waited_token, apr_waited_service;
static uint32_t apr_in, apr_out, apr_dropped, apr_trace, apr_next_token = 1;
static hakios_apr_events_t apr_events;                  /* the sound's (hakios.sound.c): what nobody waits for */
static hakios_apr_events_t apr_events_second;           /* the microphones' (hakios.mic.c) */

/* A packet from the DSP's audio services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload, kept;
	uint32_t words[64];
	bool waited;

	if (length < sizeof(header)) {
		printf("adsp845: apr: %u bytes, no APR packet\n", length);
		apr_dropped++;
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if ((header.field & 0xf) > APR_PKT_VER + 1 || header_length < sizeof(header) || header_length > length) {
		printf("adsp845: apr: a header of %u bytes (field %#x) in a packet of %u: dropped\n", header_length, header.field, length);
		apr_dropped++;
		return;
	}
	if (header.size < header_length || header.size > length) {
		printf("adsp845: apr: a packet that says it is %u bytes, with a header of %u, in %u that arrived: dropped\n", header.size,
		    header_length, length);
		apr_dropped++;
		return;
	}
	if (header.from_domain >= APR_DOMAIN_MAX || header.to_domain >= APR_DOMAIN_MAX || header.from_service >= APR_SVC_MAX ||
	    header.to_service >= APR_SVC_MAX) {
		printf("adsp845: apr: a header with a domain or a service out of range (from %u/%u to %u/%u): dropped\n", header.from_domain,
		    header.from_service, header.to_domain, header.to_service);
		apr_dropped++;
		return;
	}
	payload = header.size - header_length;
	kept = payload < sizeof(apr_answer.words) ? payload : (uint32_t)sizeof(apr_answer.words);
	apr_in++;
	if (apr_trace > 0) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		apr_trace--;
		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp845: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	lck_mtx_lock(&apr_answer_lock);
	waited = apr_waiting && !apr_answer_there &&
	    (apr_waiting_any ? header.from_service == apr_waited_service : header.token == apr_waited_token);
	if (waited) {
		bzero(apr_answer.words, sizeof(apr_answer.words));
		memcpy(apr_answer.words, data + header_length, kept);
		apr_answer.length = payload;
		apr_answer.opcode = header.opcode;
		apr_answer.token = header.token;
		apr_answer.from_service = header.from_service;
		apr_answer_there = true;
	}
	lck_mtx_unlock(&apr_answer_lock);
	if (!waited) {
		hakios_apr_events_t events = apr_events;
		uint32_t handed = kept < sizeof(words) ? kept : (uint32_t)sizeof(words);

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, handed);
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words, handed)) {
			return;
		}
		events = apr_events_second;
		if (events != NULL) {
			events(header.from_service, header.from_port, header.opcode, header.token, words, handed);
		}
	}
}

/*
 * One command to a service of the DSP, and its answer waited for with ms (0: not waited for; what comes back is the events'
 * handler's). With any_token the answer is the next packet from that service, whatever its token (the core service's: Linux's
 * q6core.c sends token 0 and takes the next answer); else the packet that carries the token (token 0: one of ours is made up).
 * True if it was sent and, if waited for, answered.
 */
static bool
apr_exchange(uint8_t service, uint16_t port, uint32_t token, bool any_token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct apr_reply *reply)
{
	uint8_t packet[sizeof(struct apr_header) + APR_SEND_MAX];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > APR_SEND_MAX || (length != 0 && payload == NULL) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0 && !any_token) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	lck_mtx_lock(&apr_answer_lock);
	apr_answer_there = false;
	apr_waited_token = token;
	apr_waited_service = service;
	apr_waiting_any = any_token;
	apr_waiting = true;
	lck_mtx_unlock(&apr_answer_lock);
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp845: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			lck_mtx_lock(&apr_answer_lock);
			if (apr_answer_there) {
				answered = true;
				if (reply != NULL) {
					*reply = apr_answer;
				}
			}
			lck_mtx_unlock(&apr_answer_lock);
			if (!answered) {
				sleep_ms(1);
			}
		}
	}
	lck_mtx_lock(&apr_answer_lock);
	apr_waiting = false;
	apr_answer_there = false;
	lck_mtx_unlock(&apr_answer_lock);
	lck_mtx_unlock(&apr_lock);
	return answered;
}

/* The reference phone's API (hakios.audio.h), as hakios.adsp.c has it: its answer has 64 words, so what is longer is cut. */
bool
hakios_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms,
    struct hakios_apr_answer *answer)
{
	struct apr_reply reply;
	bool done = apr_exchange(service, port, token, false, opcode, payload, length, ms, &reply);

	if (done && answer != NULL) {
		answer->opcode = reply.opcode;
		answer->token = reply.token;
		answer->length = reply.length;
		memcpy(answer->words, reply.words, sizeof(answer->words));
	}
	return done;
}

void
hakios_adsp_apr_events(hakios_apr_events_t handler)
{
	apr_events = handler;
}

void
hakios_adsp_apr_events_second(hakios_apr_events_t handler)
{
	apr_events_second = handler;
}

/* ---- step 11: APR, the core service ----------------------------------------------------------------- */

static const char *
apr_service_name(uint32_t id)
{
	switch (id) {              /* qcom,apr.h */
	case 3: return "core";
	case 4: return "afe";
	case 5: return "vsm";
	case 6: return "vpm";
	case 7: return "asm";
	case 8: return "adm";
	case 9: return "mvm";
	case 10: return "cvs";
	case 11: return "cvp";
	case 12: return "usm";
	case 13: return "lsm";
	case 0x16: return "vidc";
	default: return "?";
	}
}

/* The core service's list of the services and their versions (q6core.c's two structures), checked against what arrived. */
static void
apr_show_versions(const struct apr_reply *reply)
{
	uint32_t have = reply->length < sizeof(reply->words) ? reply->length / 4 : APR_WORDS, entry, first, count;

	if (reply->opcode == AVCS_CMDRSP_GET_FWK_VERSION && have >= 5) {
		/* { major, minor, branch, sub-branch, number of services } and then { service, api version, api branch version } each */
		first = 5;
		entry = 3;
		count = reply->words[4];
		printf("adsp845: 11: the DSP's framework is %u.%u.%u.%u and tells of %u services\n", reply->words[0], reply->words[1],
		    reply->words[2], reply->words[3], count);
	} else if (reply->opcode == AVCS_GET_VERSIONS_RSP && have >= 2) {
		/* { build id, number of services } and then { service, version } each */
		first = 2;
		entry = 2;
		count = reply->words[1];
		printf("adsp845: 11: the DSP's build is %#x and tells of %u services\n", reply->words[0], count);
	} else {
		printf("adsp845: 11: an answer of opcode %#x (%u bytes) that is not a list of versions\n", reply->opcode, reply->length);
		return;
	}
	if (count > (have - first) / entry) {
		printf("adsp845: 11: only %u of them are in the %u bytes kept\n", (have - first) / entry, have * 4);
		count = (have - first) / entry;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t *e = &reply->words[first + i * entry];

		if (entry == 3) {
			printf("adsp845: 11:   service %#x (%s): api %u, branch %u\n", e[0], apr_service_name(e[0]), e[1], e[2]);
		} else {
			printf("adsp845: 11:   service %#x (%s): version %u\n", e[0], apr_service_name(e[0]), e[1]);
		}
	}
}

/*
 * Why there is no audio service to talk to, said as far as what the DSP has told us goes. Linux's side of "the audio protection
 * domain": apr.c waits until the service notifier says "avs/audio" is up (pdr_add_lookup(); Linux v6.12 pdr_interface.c, which asks
 * the locator, service 0x40, where the domain is, and the notifier, service 0x42, whether it is up); the locator's answer is the
 * APSS side's own (qcom_pd_mapper.c: sdm845_domains has adsp_audio_pd, "msm/adsp/audio_pd", instance 74, service "avs/audio"):
 * the DSP asks the APSS for nothing, so no answer of ours is wanted for it, and the audio PD, if it is going to run, runs without
 * us. What we can see is whether the DSP has registered its notifier for that instance (QRTR service 66, instance 74) and which
 * channels it has opened.
 */
static void
apr_diagnose(const char *what)
{
	unsigned count = 0, notifier = 0, locator = 0;

	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (!qrtr_servers[i].there) {
			continue;
		}
		count++;
		/* (QRTR's instance word is the version in its low byte and the instance above it: 1 | 74 << 8 for the audio PD's notifier) */
		notifier += qrtr_servers[i].service == 0x42 && (qrtr_servers[i].instance >> 8) == 74;
		locator += qrtr_servers[i].service == 0x40;
	}
	printf("adsp845: 11: DIAGNOSIS: %s\n", what);
	printf("adsp845: 11:   the DSP has opened \"%s\" \"%s\" \"%s\" \"%s\" besides IPCRTR; apr_audio_svc is %s\n", glink.others[0],
	    glink.others[1], glink.others[2], glink.others[3], glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN");
	printf("adsp845: 11:   QRTR: %u services told of, among them the notifier of msm/adsp/audio_pd (service 66, instance 74): %s; the "
	    "locator (service 64): %s; the DSP looked for %u services of ours (ids %#x %#x %#x %#x)\n", count, notifier ? "THERE" : "NOT THERE",
	    locator ? "there (the DSP's own)" : "not there", qrtr_lookups, qrtr_looked_for[0], qrtr_looked_for[1], qrtr_looked_for[2],
	    qrtr_looked_for[3]);
	if (!glink.channel[CHANNEL_APR].open && !notifier) {
		printf("adsp845: 11:   no audio PD: neither its channel nor its notifier is there. The DSP has not started it (it may need more\n"
		    "adsp845: 11:   time: adsp8_apr=S; or the PD's image, the files after adsp.b13 of the DSP's, are not loaded by us), or the\n"
		    "adsp845: 11:   DSP waits for an answer of ours that this does not give (see the plan: servreg)\n");
	} else if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp845: 11:   the audio PD is registered but its APR channel is not open: the DSP may not have been asked to open it\n");
	} else {
		printf("adsp845: 11:   the channel is open but the core service does not answer: its PD may be up and not ready\n");
	}
}

static bool
step_apr(void)
{
	uint32_t wait_seconds = 15, asked = 0, state = 0;
	bool ready = false, unsupported = false;
	struct apr_reply *reply;
	uint64_t began = now_us();

	PE_parse_boot_argn("adsp8_apr", &wait_seconds, sizeof(wait_seconds));
	if (wait_seconds == 0 || wait_seconds > 600) {
		wait_seconds = 15;
	}
	if (!dsp_started || !glink.channel[CHANNEL_QRTR].open || !glink.channel[CHANNEL_APR].wanted) {
		return false;
	}
	announce("open APR's channel apr_audio_svc (answered when the DSP opens it, in step 9), then ask the core service (3) whether the "
	    "audio services are up (AVCS_CMD_ADSP_EVENT_GET_STATE, again and again, as Linux's q6core_is_adsp_ready() does) and which services "
	    "and versions it has; only GLINK data and doorbell bit 8");
	snprintf(last_write, sizeof(last_write), "step %u asked the core service over APR", step);
	apr_trace = 40;
	for (unsigned ms = 0; ms < wait_seconds * 1000 && !glink.channel[CHANNEL_APR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp845: %u: the channel \"apr_audio_svc\" is not open after %llu ms\n", step, (now_us() - began) / 1000);
		apr_diagnose("the DSP has not opened apr_audio_svc");
		return false;
	}
	printf("adsp845: %u: apr_audio_svc is OPEN after %llu ms\n", step, (now_us() - began) / 1000);
	reply = kalloc_data(sizeof(*reply), Z_WAITOK | Z_ZERO);
	if (reply == NULL) {
		return false;
	}
	while (!ready && now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_exchange(APR_SVC_ADSP_CORE, 0, 0, true, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500, reply)) {
			if (reply->opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE && reply->length >= 4) {
				state = reply->words[0];
				ready = state != 0;
			} else if (reply->opcode == APR_BASIC_RSP_RESULT && reply->length >= 8 && reply->words[0] == AVCS_CMD_ADSP_EVENT_GET_STATE &&
			    reply->words[1] == ADSP_EUNSUPPORTED) {
				/* (Linux: "assume that the adsp is up if we not support this command") */
				unsupported = ready = true;
			}
		}
		if (!ready) {
			sleep_ms(200);
		}
	}
	printf("adsp845: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x %#x)\n", step,
	    ready ? (unsupported ? "THE CORE SERVICE DOES NOT KNOW THE QUESTION (Linux takes that as up)" : "THE AUDIO SERVICES ARE UP") :
	    "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked, reply->opcode, reply->length, reply->words[0],
	    reply->words[1]);
	if (!ready) {
		apr_diagnose(apr_in == 0 ? "the core service is silent (no APR packet from the DSP at all)" :
		    "the core service answers but not that the audio services are up");
		kfree_data(reply, sizeof(*reply));
		return false;
	}
	/* Linux asks for the framework's versions first and, if that is not supported, for the services' (q6core_get_svc_api_info()) */
	if (apr_exchange(APR_SVC_ADSP_CORE, 0, 0, true, AVCS_CMD_GET_FWK_VERSION, NULL, 0, 1000, reply) &&
	    reply->opcode == AVCS_CMDRSP_GET_FWK_VERSION) {
		apr_show_versions(reply);
	} else if (apr_exchange(APR_SVC_ADSP_CORE, 0, 0, true, AVCS_GET_VERSIONS, NULL, 0, 1000, reply) &&
	    reply->opcode == AVCS_GET_VERSIONS_RSP) {
		apr_show_versions(reply);
	} else {
		printf("adsp845: %u: no list of the services' versions came (last opcode %#x, %u bytes)\n", step, reply->opcode, reply->length);
	}
	printf("adsp845: %u: APR sent %u packets and received %u (%u dropped)\n", step, apr_out, apr_in, apr_dropped);
	kfree_data(reply, sizeof(*reply));
	return true;
}

/* ---- step 12: the speaker amplifier's presence, looked for on QUP wrapper 0's engine 4 (read-only) ---------- */

/*
 * The speaker of the 6T needs no codec (WCD9340, SLIMbus) -- mainline's sdm845-oneplus-common.dtsi:629-638 has a dai-link of the
 * DSP's QUATERNARY_MI2S_RX with no codec behind it -- but an amplifier: the downstream tree (ref-linux/oneplus/dts-fajita.dtsi:
 * 369-377, selected by op,smartpa = "tfa98xx" at :65; the MAX98927 of :663-672 is "disabled") has a tfa98xx@34, compatible
 * "nxp,tfa9894", on &qupv3_se4_i2c, reset-gpio <&tlmm 69 0>, no interrupt line. &qupv3_se4_i2c is the SoC's QUP wrapper 0, engine 4:
 * ref-linux/sdm845.dtsi:1504 (i2c4: i2c@890000, clock GCC_QUPV3_WRAP0_S4_CLK, pins qup_i2c4_default gpio89 and gpio90, function
 * "qup4": sdm845.dtsi:2856-2858; Linux pinctrl-sdm845.c has it as the second function of gpio89 (value 2) and the third of gpio90 (3)).
 *
 * What this step does, in this order, each part saying what it touches before it does:
 *   a  READS the global clock controller (GCC, 0x00100000): the vote register 0x5200c and the status words of the three clocks the
 *      engine needs -- wrapper 0's M_AHB (halt 0x17004, vote bit 6) and S_AHB (halt 0x17008, bit 7), and the engine's own (halt
 *      0x174f0, bit 14), and the engine's root clock (command 0x174f4, config 0x174f8) -- gcc-sdm845.c:2229-2245, 2445-2471, 524-531.
 *      A halt word's bit 31 is "the clock is off" (a branch's CBCR; hakios.rmi4.c reads the other wrapper's the same way)
 *   b  READS the pins the speaker's path uses, writing none: TLMM's control and I/O words of gpio58, 59, 60, 61 (the quaternary
 *      MI2S: clock, word select, data 0, data 1: sdm845.dtsi:3226-3255), gpio69 (the amplifier's reset: the NXP driver's default
 *      polarity makes it active HIGH: it sets the line low, then pulses high for 1 ms, then low: tfa98xx_ext_reset()) and
 *      gpio89 and gpio90 (the engine's I2C)
 *   c  if any of the three clocks is OFF, it says so and STOPS: nothing of the serial engine is read or written (on this phone a
 *      register of a block whose clock is off makes the Config-NoC report an error and the phone goes to CrashDump). Switching a
 *      clock on is a write the 6T has not had from this driver (GCC vote and root clock), and is a step of its own: the plan says
 *      what it would be
 *   d  if they are on, and the root clock runs from the 19.2 MHz crystal (the timing table below is for it): WRITES the serial
 *      engine's own registers to set it up as an I2C master the way hakios.rmi4.c does for the touch screen's engine (FIFO mode,
 *      polled, no interrupt; the constants are Linux's i2c-qcom-geni.c table for 19.2 MHz, at 100 kHz), checks the engine's
 *      firmware says I2C, and READS the amplifier's revision register 0x03 at address 0x34: a write of the register's number (one
 *      byte), then a read of two (the NXP driver's regmap: 8-bit registers, 16-bit values, most significant byte first; its
 *      probe takes "reg & 0xff" == 0x94 for the TFA9894: tfa98xx_probe(), a switch of revisions; written again here from that
 *      table, not copied). Nothing is written to the amplifier except that register's number; no pin, clock or regulator is changed
 * An answer of no acknowledgement is the likely first one: the amplifier's supply and reset are not ours yet. The line of gpio69
 * and of the two I2C pins, read before, say which of the three it is.
 */
#define GCC_PHYS                0x00100000u
#define GCC_SIZE                0x00060000u
#define GCC_VOTE_WORD           0x5200c
#define GCC_QUP0_M_AHB_HALT     0x17004
#define GCC_QUP0_S_AHB_HALT     0x17008
#define GCC_QUP0_S4_HALT        0x174f0
#define GCC_QUP0_S4_CMD         0x174f4
#define GCC_QUP0_S4_CFG         0x174f8
#define TLMM_EAST               0x03500000u     /* pinctrl-sdm845.c:13-15: the tiles, a pin's page is the tile + 0x1000 * its number */
#define TLMM_NORTH              0x03900000u
#define TLMM_SOUTH              0x03d00000u
#define SE4_PHYS                0x00890000u     /* sdm845.dtsi:1504 i2c@890000, size 0x4000 */
#define SE4_SIZE                0x00004000u
#define TFA_ADDRESS             0x34            /* dts-fajita.dtsi:369-377 */
#define TFA_REVISION            0x03            /* the NXP driver's tfa98xx_probe(): regmap_read(0x03), (value & 0xff) == 0x94 for the TFA9894 */
#define TFA_REVISION_9894       0x94

/* two pages of the GCC, not its 384 KB: so late in the start the kernel's room for mappings is short (hakios.bt3990.c, run 82:
 * a mapping of 0x60000 refused, a panic) */
static struct block gcc_vote_block = { "GCC", GCC_PHYS + 0x52000u, 0x1000, 0 };
static struct block gcc_qup_block = { "GCC", GCC_PHYS + 0x17000u, 0x1000, 0 };
static struct block se4_block = { "SE4", SE4_PHYS, SE4_SIZE, 0 };

struct pin_look {
	struct block page;
	unsigned number, function;              /* the function it should have for the speaker's path (0: plain GPIO) */
	const char *what;
};
static struct pin_look pins[] = {
	{ { "gpio58", TLMM_NORTH + 0x1000u * 58, 0x1000, 0 }, 58, 1, "quaternary MI2S clock (qua_mi2s)" },
	{ { "gpio59", TLMM_NORTH + 0x1000u * 59, 0x1000, 0 }, 59, 1, "quaternary MI2S word select (qua_mi2s)" },
	{ { "gpio60", TLMM_NORTH + 0x1000u * 60, 0x1000, 0 }, 60, 1, "quaternary MI2S data 0 (qua_mi2s)" },
	{ { "gpio61", TLMM_NORTH + 0x1000u * 61, 0x1000, 0 }, 61, 1, "quaternary MI2S data 1 (qua_mi2s)" },
	{ { "gpio69", TLMM_EAST + 0x1000u * 69, 0x1000, 0 }, 69, 0, "the amplifier's reset (GPIO; active high)" },
	{ { "gpio89", TLMM_SOUTH + 0x1000u * 89, 0x1000, 0 }, 89, 2, "engine 4's I2C data or clock (qup4)" },
	{ { "gpio90", TLMM_SOUTH + 0x1000u * 90, 0x1000, 0 }, 90, 3, "engine 4's I2C clock or data (qup4)" },
};

/* A pin's control word: [1:0] pull, [5:2] function, [8:6] drive ((value + 1) * 2 mA), [9] output enable; its I/O word: [0] level in, [1] level out. */
static void
pin_show(struct pin_look *p)
{
	uint32_t ctl = get(&p->page, "control", 0), io = get(&p->page, "I/O", 4);
	static const char *const pulls[] = { "no pull", "pull down", "keeper", "pull up" };

	printf("adsp845: %u:   gpio%u (%s): function %u (%s), %s, %u mA, %s; reads %u, would drive %u\n", step, p->number, p->what,
	    (ctl >> 2) & 0xf, ((ctl >> 2) & 0xf) == p->function ? "as the speaker's path needs it" : "NOT what the speaker's path needs",
	    pulls[ctl & 3], (((ctl >> 6) & 7) + 1) * 2, (ctl & (1u << 9)) ? "an output" : "an input", io & 1, (io >> 1) & 1);
}

/* the serial engine's registers: the same as hakios.rmi4.c's (Linux's qcom-geni-se.h, i2c-qcom-geni.c) */
#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define SE_GENI_STATUS          0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_IF_DISABLE_RO      0x064
#define GENI_FW_REVISION_RO     0x068
#define SE_GENI_CLK_SEL         0x07c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_I2C_TX_TRANS_LEN     0x26c
#define SE_I2C_RX_TRANS_LEN     0x270
#define SE_I2C_SCL_COUNTERS     0x278
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL      0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_TX_FIFO         0x700
#define SE_GENI_RX_FIFO         0x780
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_DMA_GENERAL_CFG      0xe30
#define M_CMD_DONE              (1u << 0)
#define M_CMD_OVERRUN           (1u << 1)
#define M_ILLEGAL_CMD           (1u << 2)
#define M_CMD_FAILURE           (1u << 3)
#define M_CMD_ABORT             (1u << 5)
#define M_GP_IRQ_NACK           (1u << 10)
#define M_GP_IRQ_BUS_ERROR      (1u << 13)
#define M_RX_FIFO_WATERMARK     (1u << 26)
#define M_RX_FIFO_LAST          (1u << 27)
#define M_TX_FIFO_WATERMARK     (1u << 30)
#define M_ERRORS                (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_CMD_ABORT | M_GP_IRQ_NACK | M_GP_IRQ_BUS_ERROR)
#define I2C_WRITE               1u
#define I2C_READ                2u
#define I2C_TIMEOUT_US          20000

static void
se4_abort(void)
{
	vm_offset_t se = se4_block.base;

	wr(se + SE_GENI_M_CMD_CTRL, 2);
	for (int i = 0; i < 1000 && !(rd(se + SE_GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
		sleep_us(10);
	}
	while (rd(se + SE_GENI_RX_FIFO_STATUS) & 0x1ffffff) {
		(void)rd(se + SE_GENI_RX_FIFO);
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer to address 0x34: a write of `length` bytes, or a read of them. The engine's M_IRQ_STATUS at the end is in *status. 0, or an error number. */
static int
se4_transfer(bool read, uint8_t *bytes, unsigned length, uint32_t *status)
{
	vm_offset_t se = se4_block.base;
	uint64_t start = mach_absolute_time(), limit;
	unsigned got = 0;

	nanoseconds_to_absolutetime((uint64_t)I2C_TIMEOUT_US * NSEC_PER_USEC, &limit);
	for (int i = 0; rd(se + SE_GENI_STATUS) & 1; i++) {     /* a command still active */
		if (i > 200) {
			se4_abort();
			break;
		}
		sleep_us(10);
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_I2C_TX_TRANS_LEN, read ? 0 : length);
	wr(se + SE_I2C_RX_TRANS_LEN, read ? length : 0);
	wr(se + SE_GENI_M_CMD0, (read ? I2C_READ : I2C_WRITE) << 27 | TFA_ADDRESS << 9);
	for (unsigned i = 0; !read && i < length; i += 4) {
		uint32_t word = 0;

		for (unsigned b = 0; b < 4 && i + b < length; b++) {
			word |= (uint32_t)bytes[i + b] << (8 * b);
		}
		wr(se + SE_GENI_TX_FIFO, word);
	}
	for (;;) {
		*status = rd(se + SE_GENI_M_IRQ_STATUS);
		for (uint32_t words = read ? rd(se + SE_GENI_RX_FIFO_STATUS) & 0x1ffffff : 0; words > 0; words--) {
			uint32_t word = rd(se + SE_GENI_RX_FIFO);

			for (unsigned b = 0; b < 4 && got < length; b++) {
				bytes[got++] = (uint8_t)(word >> (8 * b));
			}
		}
		if (*status & (M_CMD_DONE | M_ERRORS)) {
			break;
		}
		if (mach_absolute_time() - start > limit) {
			*status |= 1u << 31;
			break;
		}
	}
	if (read && (*status & M_CMD_DONE)) {                   /* what came in with the end */
		for (uint32_t words = rd(se + SE_GENI_RX_FIFO_STATUS) & 0x1ffffff; words > 0; words--) {
			uint32_t word = rd(se + SE_GENI_RX_FIFO);

			for (unsigned b = 0; b < 4 && got < length; b++) {
				bytes[got++] = (uint8_t)(word >> (8 * b));
			}
		}
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, *status);
	if (*status & (1u << 31)) {
		se4_abort();
		return ETIMEDOUT;
	}
	if (*status & M_GP_IRQ_NACK) {
		return ENXIO;
	}
	if ((*status & M_ERRORS) || !(*status & M_CMD_DONE) || (read && got < length)) {
		return EIO;
	}
	return 0;
}

static bool
step_tfa(void)
{
	uint32_t vote, m_ahb, s_ahb, s4, cmd, cfg, fw, depth, status = 0;
	bool clocks_on;
	uint8_t reg = TFA_REVISION, value[2] = { 0, 0 };
	int error;
	vm_offset_t se;

	announce("READ the global clock controller's status of QUP wrapper 0's AHB clocks and engine 4's (nothing written), READ the "
	    "control words of gpio58-61, 69, 89 and 90 (nothing written); if the three clocks run: set up engine 4 as an I2C master "
	    "(its own registers) and read the speaker amplifier's revision, register 0x03 at address 0x34");
	snprintf(last_write, sizeof(last_write), "step %u wrote nothing (it only read)", step);
	vote = get(&gcc_vote_block, "vote word", GCC_VOTE_WORD - 0x52000u);
	m_ahb = get(&gcc_qup_block, "QUP wrapper 0 M_AHB halt", GCC_QUP0_M_AHB_HALT - 0x17000u);
	s_ahb = get(&gcc_qup_block, "QUP wrapper 0 S_AHB halt", GCC_QUP0_S_AHB_HALT - 0x17000u);
	s4 = get(&gcc_qup_block, "engine 4 halt", GCC_QUP0_S4_HALT - 0x17000u);
	cmd = get(&gcc_qup_block, "engine 4 root command", GCC_QUP0_S4_CMD - 0x17000u);
	cfg = get(&gcc_qup_block, "engine 4 root config", GCC_QUP0_S4_CFG - 0x17000u);
	clocks_on = !(m_ahb >> 31) && !(s_ahb >> 31) && !(s4 >> 31);
	printf("adsp845: %u: the clocks of engine 4: M_AHB %s, S_AHB %s, the engine's own %s (the vote word %#x: bits 6, 7 and 14 are %u %u %u; bits 8 and 9, the wrapper's core clocks, "
	    "by the gap in gcc-sdm845.c's list that is wrapper 1's 18 and 19 of hakios.rmi4.c: %u %u)\n",
	    step, (m_ahb >> 31) ? "OFF" : "on", (s_ahb >> 31) ? "OFF" : "on", (s4 >> 31) ? "OFF" : "on", vote, (vote >> 6) & 1,
	    (vote >> 7) & 1, (vote >> 14) & 1, (vote >> 8) & 1, (vote >> 9) & 1);
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		pin_show(&pins[i]);
	}
	if (!clocks_on) {
		printf("adsp845: %u: ENGINE 4'S CLOCKS ARE NOT ALL RUNNING: the serial engine is not touched (its registers behind a clock that is off "
		    "would take the phone to CrashDump). Nothing was written. Turning them on is a separate step: see the plan (B2-test-plan.md)\n",
		    step);
		return true;    /* (a look, done: step 13 reaches the amplifier without the engine) */
	}
	if ((cfg & 0x371fu) > 1) {
		/* [4:0] divider (0 and 1: by one), [10:8] source (0: the 19.2 MHz crystal), [13:12] mode (0: bypass) */
		printf("adsp845: %u: engine 4's root clock is not the 19.2 MHz crystal undivided (command %#x, config %#x): the bus timing table here "
		    "would be wrong; the serial engine is not touched\n", step, cmd, cfg);
		return false;
	}
	need(&se4_block);
	se = se4_block.base;
	fw = get(&se4_block, "firmware revision", GENI_FW_REVISION_RO);
	depth = (get(&se4_block, "hardware parameter 0", SE_HW_PARAM_0) >> 24) & 0x3f;
	printf("adsp845: %u: engine 4: protocol %u (3 is I2C), FIFO %s, depth %u words, status %#x\n", step, (fw >> 8) & 0xff,
	    (rd(se + GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "on", depth, rd(se + SE_GENI_STATUS));
	if (((fw >> 8) & 0xff) != 3 || (rd(se + GENI_IF_DISABLE_RO) & 1)) {
		printf("adsp845: %u: engine 4 is not an I2C engine with a FIFO: nothing more is done\n", step);
		return false;
	}
	/*
	 * (S0, the lane's sound-shortest-path.md: writing engine 4 is a kind of write the user has not agreed to on the 6T, and
	 * the plan goes by a bit-banged I2C instead; so it waits for adsp8_se4=1, given only once agreed)
	 */
	{
		uint32_t se4 = 0;

		PE_parse_boot_argn("adsp8_se4", &se4, sizeof(se4));
		if (se4 == 0) {
			printf("adsp845: %u: engine 4's registers would be written next: not without adsp8_se4=1 (not agreed to): nothing more is done\n",
			    step);
			return true;    /* (a look, done: step 13 reaches the amplifier without the engine) */
		}
	}
	printf("adsp845: %u: about to write engine 4's own setup registers (as hakios.rmi4.c does the touch engine's): FIFO mode, 100 kHz at 19.2 MHz\n",
	    step);
	snprintf(last_write, sizeof(last_write), "step %u set up engine 4's registers (0x890000) and read 0x34 reg 0x03", step);
	wr(se + GENI_CGC_CTRL, rd(se + GENI_CGC_CTRL) | 0x7f);
	wr(se + SE_DMA_GENERAL_CFG, rd(se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(se + GENI_OUTPUT_CTRL, 0x7f);
	wr(se + GENI_FORCE_DEFAULT, 1);
	wr(se + SE_GENI_DMA_MODE_EN, 0);
	wr(se + SE_GSI_EVENT_EN, 0);
	wr(se + SE_IRQ_EN, 0);                                  /* nothing reaches the interrupt controller */
	wr(se + SE_GENI_M_IRQ_EN, M_CMD_DONE | M_TX_FIFO_WATERMARK | M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST | M_ERRORS);
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_GENI_BYTE_GRAN, 0);
	wr(se + SE_GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(se + SE_GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(se + SE_GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(se + SE_GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(se + SE_GENI_TX_WATERMARK, 1);
	wr(se + SE_GENI_RX_WATERMARK, 0);
	wr(se + SE_GENI_RX_RFR_WATERMARK, depth ? depth - 1 : 31);
	wr(se + SE_GENI_CLK_SEL, 0);
	wr(se + GENI_SER_M_CLK_CFG, 7u << 4 | 1);               /* divider 7, enabled: i2c-qcom-geni.c's 100 kHz row for 19.2 MHz */
	wr(se + SE_I2C_SCL_COUNTERS, 10u << 20 | 11u << 10 | 26);
	error = se4_transfer(false, &reg, 1, &status);
	if (error == 0) {
		error = se4_transfer(true, value, 2, &status);
	}
	if (error != 0) {
		printf("adsp845: %u: NO ANSWER FROM 0x%x: %s (the engine's status %#x). Its supply or reset (gpio69, above) or the pins' mux (gpio89 and "
		    "gpio90, above) are the places to look; nothing was changed to find out\n", step, TFA_ADDRESS,
		    error == ENXIO ? "no acknowledgement" : error == ETIMEDOUT ? "the bus did not finish" : "the engine reports an error", status);
		return true;
	}
	printf("adsp845: %u: THE AMPLIFIER ANSWERS: register 0x%02x = %#06x; %s\n", step, TFA_REVISION, (unsigned)(value[0] << 8 | value[1]),
	    value[1] == TFA_REVISION_9894 ? "a TFA9894 (the revision ends in 0x94: the NXP driver knows 0x0a94, 0x1a94, 0x2a94, 0x3a94), as the device tree says" :
	    "NOT a TFA9894 (it would end in 0x94)");
	return value[1] == TFA_REVISION_9894;
}

/* ---- step 13: the speaker amplifier, reached by I2C made by hand on its two pins ------------------------------ */

/*
 * S3 of lane-b-notes/sound-shortest-path.md (agreed to by the user, 11-13: the pins gpio89, 90 and 69, and reading the
 * amplifier). Engine 4 is not used: its clocks are off and turning them on is the GCC's (not agreed to); the two pins of its
 * I2C are made plain GPIOs for the while and driven as an open-drain bus by hand (a line is let go -- an input with its pull-up --
 * for a one, and pulled low -- an output at 0 -- for a zero), about 50 kHz. gpio89 is taken for the data and gpio90 for the clock,
 * QUP's order (the other way round is tried too if nothing answers). What is said to the amplifier (address 0x34): the number of a
 * register to read, then a read of two bytes (8-bit registers, 16-bit values, most significant byte first: the NXP driver's regmap).
 *   a  READS gpio89, 90 and 69's control and I/O words (kept, to be given back)
 *   b  WRITES gpio89 and 90 to plain GPIO inputs with pull-ups; if either line reads low the bus is not idle: given back, stop
 *   c  reads register 0x03 (the revision: 0x?a94 for a TFA9894) -- the ladder, until something answers: as found; gpio69 (the
 *      reset) WRITTEN low (the NXP driver's "running"), 2 ms; a pulse, high 1 ms then low, 2 ms; gpio69 high (the other polarity),
 *      2 ms. Nothing answering at all: said, and nothing more (the supply would be the PMIC's: not agreed to, not done)
 *   d  if it answers: READS registers 0x00-0x04, 0x10, 0x20-0x24, 0x50, 0x52, 0x57, 0x71, each compared with its value after a
 *      reset where the NXP sources say it (0x00 0x8245, 0x02 0x55c8, 0x52 0xb617, 0x57 0x0366)
 *   e  gives gpio89, 90 and 69 back as they were found
 * Nothing is written to the amplifier but the numbers of the registers read.
 */
#define BB_HALF_US      10u                     /* half a bit: 10 us, about 50 kHz */

struct bitbang {
	struct pin_look *sda, *scl;
};

static void
bb_line(struct pin_look *p, bool high)
{
	vm_offset_t ctl = p->page.base, io = p->page.base + 4;

	if (high) {
		wr(ctl, 3);                             /* GPIO, input, pull-up: the line let go */
	} else {
		wr(io, 0);                              /* the level it would drive: 0 */
		wr(ctl, 3u | 1u << 9);                  /* ... and an output */
	}
	__builtin_arm_dsb(0xf);
	delay(BB_HALF_US);
}

static bool
bb_level(struct pin_look *p)
{
	return rd(p->page.base + 4) & 1;
}

static void
bb_start(struct bitbang *b)
{
	bb_line(b->sda, true);
	bb_line(b->scl, true);
	bb_line(b->sda, false);
	bb_line(b->scl, false);
}

static void
bb_stop(struct bitbang *b)
{
	bb_line(b->sda, false);
	bb_line(b->scl, true);
	bb_line(b->sda, true);
}

/* a byte out; true if the other side pulled the data low on the ninth clock */
static bool
bb_write(struct bitbang *b, uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		bb_line(b->sda, (byte >> bit) & 1);
		bb_line(b->scl, true);
		for (unsigned stretch = 0; stretch < 100 && !bb_level(b->scl); stretch++) {
			delay(BB_HALF_US);
		}
		bb_line(b->scl, false);
	}
	bb_line(b->sda, true);
	bb_line(b->scl, true);
	ack = !bb_level(b->sda);
	bb_line(b->scl, false);
	return ack;
}

static uint8_t
bb_read(struct bitbang *b, bool ack)
{
	uint8_t byte = 0;

	bb_line(b->sda, true);
	for (int bit = 7; bit >= 0; bit--) {
		bb_line(b->scl, true);
		byte = (uint8_t)(byte << 1 | (bb_level(b->sda) ? 1 : 0));
		bb_line(b->scl, false);
	}
	bb_line(b->sda, !ack);
	bb_line(b->scl, true);
	bb_line(b->scl, false);
	bb_line(b->sda, true);
	return byte;
}

/* register reg of the amplifier: true and its value if it answered */
static bool
tfa_read(struct bitbang *b, uint8_t reg, uint16_t *value)
{
	bool ok;
	uint8_t high, low;

	bb_start(b);
	ok = bb_write(b, TFA_ADDRESS << 1) && bb_write(b, reg);
	if (ok) {
		bb_start(b);                            /* a repeated start */
		ok = bb_write(b, TFA_ADDRESS << 1 | 1);
		if (ok) {
			high = bb_read(b, true);
			low = bb_read(b, false);
			*value = (uint16_t)(high << 8 | low);
		}
	}
	bb_stop(b);
	return ok;
}

static bool
step_amplifier(void)
{
	struct pin_look *reset = &pins[4], *p89 = &pins[5], *p90 = &pins[6];
	uint32_t ctl89, io89, ctl90, io90, ctl69, io69;
	struct bitbang bus = { p89, p90 };
	uint16_t revision = 0;
	bool answered = false;
	unsigned rung = 0;

	announce("READ gpio89, 90 and 69; WRITE gpio89 and 90 to plain GPIOs (I2C by hand, the bus to the speaker amplifier) and, "
	    "only if it does not answer as found, gpio69 (its reset); READ the amplifier's registers; give the three pins back");
	ctl89 = get(&p89->page, "gpio89 control", 0);
	io89 = get(&p89->page, "gpio89 I/O", 4);
	ctl90 = get(&p90->page, "gpio90 control", 0);
	io90 = get(&p90->page, "gpio90 I/O", 4);
	ctl69 = get(&reset->page, "gpio69 control", 0);
	io69 = get(&reset->page, "gpio69 I/O", 4);
	printf("adsp845: %u: about to make gpio89 and gpio90 GPIO inputs with pull-ups (control 0x3), the bus let go\n", step);
	snprintf(last_write, sizeof(last_write), "step %u wrote gpio89/90 (I2C by hand) and gave them back", step);
	bb_line(p89, true);
	bb_line(p90, true);
	delay(50);
	if (!bb_level(p89) || !bb_level(p90)) {
		printf("adsp845: %u: THE BUS IS NOT IDLE (gpio89 reads %u, gpio90 %u, both let go): given back, nothing said on it\n", step,
		    bb_level(p89), bb_level(p90));
		goto give_back;
	}
	for (rung = 0; rung < 4 && !answered; rung++) {
		if (rung == 1) {
			printf("adsp845: %u: about to write gpio69 a GPIO output, LOW (the NXP driver's \"running\"), then wait 2 ms\n", step);
			wr(reset->page.base + 4, 0);
			wr(reset->page.base, 1u << 9);
			delay(2000);
		} else if (rung == 2) {
			printf("adsp845: %u: about to pulse gpio69: HIGH 1 ms, then LOW, then wait 2 ms (tfa98xx_ext_reset())\n", step);
			wr(reset->page.base + 4, 2);
			delay(1000);
			wr(reset->page.base + 4, 0);
			delay(2000);
		} else if (rung == 3) {
			printf("adsp845: %u: about to write gpio69 HIGH (the other polarity), then wait 2 ms\n", step);
			wr(reset->page.base + 4, 2);
			delay(2000);
		}
		for (unsigned order = 0; order < 2 && !answered; order++) {
			bus.sda = order ? p90 : p89;
			bus.scl = order ? p89 : p90;
			answered = tfa_read(&bus, TFA_REVISION, &revision);
			printf("adsp845: %u: rung %u, data on gpio%u: register 0x03 %s\n", step, rung, bus.sda->number,
			    answered ? "ANSWERED" : "no acknowledgement");
		}
	}
	if (!answered) {
		printf("adsp845: %u: THE AMPLIFIER DOES NOT ANSWER at 0x34 on any rung (as found, reset low, a reset pulse, reset high), either "
		    "pin order: its supply may be off (the PMIC's: not agreed to, not done). Nothing more\n", step);
		goto give_back;
	}
	printf("adsp845: %u: THE AMPLIFIER ANSWERS: register 0x03 = %#06x (%s; data on gpio%u, rung %u)\n", step, revision,
	    (revision & 0xff) == TFA_REVISION_9894 ? "a TFA9894" : "NOT a TFA9894's 0x?a94", bus.sda->number, rung - 1);
	{
		static const struct { uint8_t reg; uint16_t por; } regs[] = {
			{ 0x00, 0x8245 }, { 0x01, 0 }, { 0x02, 0x55c8 }, { 0x03, 0 }, { 0x04, 0 }, { 0x10, 0 }, { 0x20, 0 }, { 0x21, 0 },
			{ 0x22, 0 }, { 0x23, 0 }, { 0x24, 0 }, { 0x50, 0 }, { 0x52, 0xb617 }, { 0x57, 0x0366 }, { 0x71, 0 },
		};

		for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
			uint16_t v = 0;
			bool ok = tfa_read(&bus, regs[i].reg, &v);

			printf("adsp845: %u:   reg %#04x = %s%#06x%s\n", step, regs[i].reg, ok ? "" : "(NO ACK) ", v,
			    regs[i].por == 0 ? "" : v == regs[i].por ? " (its value after a reset)" : " (NOT its value after a reset)");
		}
	}
give_back:
	printf("adsp845: %u: about to give gpio89, 90 and 69 back as they were found (control %#x %#x %#x)\n", step, ctl89, ctl90, ctl69);
	wr(p89->page.base + 4, io89 & 2);
	wr(p89->page.base, ctl89);
	wr(p90->page.base + 4, io90 & 2);
	wr(p90->page.base, ctl90);
	wr(reset->page.base + 4, io69 & 2);
	wr(reset->page.base, ctl69);
	printf("adsp845: %u: bus given back\n", step);
	return answered;
}

/* ---- step 14: the AFE port of the speaker, started and stopped, the amplifier left off -------------------------- */

/*
 * S4 of lane-b-notes/sound-shortest-path.md (agreed to by the user, 11-13: the DSP's LPASS clock and AFE port, the pins of the
 * quaternary MI2S). The speaker's samples leave the SoC on the quaternary MI2S (Linux's QUATERNARY_MI2S_RX, AFE port 0x1006: mainline
 * sdm845-oneplus-common.dtsi:629-638), data on SD1 (gpio61); the DSP makes its bit clock (QUAD_MI2S_IBIT, clock 0x106) at 48 kHz x
 * 16 bits x 2 = 1.536 MHz. In order, each answered with status 0 before the next: the clock on (AFE_SVC_CMD_SET_PARAM), the port's
 * I2S configuration (AFE_PORT_CMD_SET_PARAM_V2), the four pins to the interface (function 1, 8 mA; clock and word select output
 * high), the port started; the clock and word-select pins watched for a while (they must toggle); then the port stopped, the clock
 * off, the pins given back. The amplifier is not touched: nothing can be heard.
 */
#define S_AFE                   4
#define AFE_PORT_CMD_DEVICE_START       0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP        0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2       0x000100efu
#define AFE_SVC_CMD_SET_PARAM           0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE  0x0001020cu
#define AFE_PARAM_ID_I2S_CONFIG         0x0001020du
#define AFE_MODULE_CLOCK_SET            0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET          0x00010290u
#define QUAT_MI2S_RX            0x1006
#define CLOCK_QUAD_MI2S_IBIT    0x106
#define MI2S_RATE               48000u
#define MI2S_BITS               16u

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_i2s_config {
	uint32_t version;
	uint16_t bits, channel_mode, mono_stereo, ws_source;
	uint32_t rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

/* one AFE command, its basic answer waited for: true if status 0 */
static bool
afe_command(const char *what, uint32_t opcode, const void *payload, uint32_t length)
{
	struct apr_reply reply;

	printf("adsp845: %u: AFE: %s (opcode %#x)\n", step, what, opcode);
	snprintf(last_write, sizeof(last_write), "step %u asked the AFE: %s", step, what);
	if (!apr_exchange(S_AFE, 0, 0x14, false, opcode, payload, length, 1000, &reply)) {
		printf("adsp845: %u: AFE: %s: NO ANSWER\n", step, what);
		return false;
	}
	if (reply.opcode == APR_BASIC_RSP_RESULT && reply.length >= 8 && reply.words[0] == opcode) {
		printf("adsp845: %u: AFE: %s: %s (status %#x)\n", step, what, reply.words[1] == 0 ? "done" : "REFUSED", reply.words[1]);
		return reply.words[1] == 0;
	}
	printf("adsp845: %u: AFE: %s: an answer not expected: opcode %#x, %u bytes\n", step, what, reply.opcode, reply.length);
	return false;
}

static bool
afe_param_send(const char *what, bool port, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 32];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	if (port) {
		*(uint16_t *)packet = QUAT_MI2S_RX;
		*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	} else {
		*(uint32_t *)packet = (uint32_t)sizeof(*p) + size;      /* in band: address 0, handle 0 */
	}
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return afe_command(what, port ? AFE_PORT_CMD_SET_PARAM_V2 : AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size);
}

static bool
afe_mi2s_clock(bool on)
{
	struct afe_clock_set clock = { 1, CLOCK_QUAD_MI2S_IBIT, on ? MI2S_RATE * MI2S_BITS * 2 : 0, 1, 0, on };

	return afe_param_send(on ? "the quaternary MI2S bit clock (0x106) at 1.536 MHz, on" : "the quaternary MI2S bit clock, off", false,
	           AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static unsigned
pin_toggles(struct pin_look *p, uint32_t *ones)
{
	uint32_t last = rd(p->page.base + 4) & 1, changes = 0;

	*ones = 0;
	for (uint32_t i = 0; i < 20000; i++) {
		uint32_t now = rd(p->page.base + 4) & 1;

		changes += now != last;
		*ones += now;
		last = now;
	}
	return changes;
}

static bool
step_afe(void)
{
	struct afe_i2s_config i2s = { 1, MI2S_BITS, 2 /* SD1 */, 1 /* stereo */, 1 /* internal */, MI2S_RATE, 0, 0 };
	uint16_t port[2] = { QUAT_MI2S_RX, 0 };
	uint32_t found_ctl[4], found_io[4];
	bool clock = false, started = false, ok = false;

	announce("ASK the DSP (AFE) for the quaternary MI2S bit clock and port 0x1006 (I2S, 16 bits, stereo on SD1, 48 kHz), WRITE gpio58-61 "
	    "to the interface, START the port, watch the clock pins, then STOP it, the clock off, the pins given back (the amplifier untouched)");
	for (unsigned i = 0; i < 4; i++) {
		found_ctl[i] = get(&pins[i].page, "control", 0);
		found_io[i] = get(&pins[i].page, "I/O", 4);
	}
	clock = afe_mi2s_clock(true);
	if (!clock || !afe_param_send("the port 0x1006: I2S, 16 bits, stereo on SD1, word select internal, 48 kHz", true,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s))) {
		goto off;
	}
	for (unsigned i = 0; i < 4; i++) {
		bool driven = i < 2;                    /* the bit clock and the word select: outputs, high */

		if (driven) {
			put(&pins[i].page, "I/O", 4, 2);
		}
		put(&pins[i].page, "control", 0, 1u << 2 | 3u << 6 | (driven ? 1u << 9 : 0));
	}
	started = afe_command("the port 0x1006, started", AFE_PORT_CMD_DEVICE_START, port, sizeof(port));
	if (started) {
		uint32_t ones_clk, ones_ws;
		unsigned clk, ws;

		delay(20000);
		clk = pin_toggles(&pins[0], &ones_clk);
		ws = pin_toggles(&pins[1], &ones_ws);
		printf("adsp845: %u: the pins while the port runs: the bit clock (gpio58) %u changes (%u ones), the word select (gpio59) %u changes "
		    "(%u ones), in 20000 reads each\n", step, clk, ones_clk, ws, ones_ws);
		ok = clk > 0 && ws > 0;
		printf("adsp845: %u: %s\n", step, ok ? "THE QUATERNARY MI2S RUNS: its clock and word select toggle at the pins" :
		    "THE PORT STARTED BUT THE PINS DO NOT TOGGLE (the pins' function or the DSP's clock)");
		afe_command("the port 0x1006, stopped", AFE_PORT_CMD_DEVICE_STOP, port, sizeof(port));
	}
off:
	if (clock) {
		afe_mi2s_clock(false);
	}
	for (unsigned i = 0; i < 4; i++) {
		put(&pins[i].page, "I/O (given back)", 4, found_io[i] & 2);
		put(&pins[i].page, "control (given back)", 0, found_ctl[i]);
	}
	return ok;
}

/* ---- step 15: a stream of ours to the speaker's port, a quiet tone through it, the amplifier left off ----------- */

/*
 * S5 of lane-b-notes/sound-shortest-path.md (agreed to by the user, 11-13: SMMU stream 0x1821; the AFE as in step 14). After the
 * port of step 14 is started again: our memory shown to the DSP through the application processors' SMMU as stream 0x1821 (Android's
 * qcom,msm-audio-ion: iommus <&apps_smmu 0x1821 0>; the stream's low four bits, 1, in bits 32 to 35 of every address given to the DSP)
 * through a context of ours with page tables of ours (hakios.sound.c's way), a device path (ADM, COPP) to port 0x1006 of 16 bits,
 * an ASM stream routed to it, and a 1 kHz tone at -40 dBFS written into it for two seconds; the DSP's "written" events counted,
 * the data pin (gpio61) watched. The amplifier stays off: nothing to hear. Then all closed, stopped, and given back.
 */
#define S_ASM                   7
#define S_ADM                   8
#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW          0x00010c17u
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define S5_STREAM_PORT          (1 << 8 | 1)    /* session 1, stream 1 */
#define S5_WRITE_TOKEN          0x80000000u
#define S5_PERIODS              8
#define S5_PERIOD_FRAMES        960             /* 20 ms */
#define S5_PERIOD_BYTES         (S5_PERIOD_FRAMES * 4)
#define S5_BYTES                (S5_PERIODS * 4096)
#define AUDIO_STREAM_6T         0x1821
#define AUDIO_ADDRESSES_6T      0x10000000ull
#define AUDIO_STREAM_BITS_6T    (1ull << 32)    /* 0x1821 & 0xf */

extern lck_mtx_t hakios_smmu_lock;

#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define AUDIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

static struct block s5_smmu = { "apps smmu", 0x15000000, 0x2000, 0 };
static struct block s5_smmu_second = { "apps smmu", 0, 0x1000, 0 };
static struct block s5_smmu_context = { "apps smmu, the audio context", 0, 0x1000, 0 };
static struct {
	bool on, passed;
	uint32_t match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;
} s5_space;
static vm_offset_t s5_buffer;
static uint64_t s5_address;
static volatile uint32_t s5_returned, s5_done, s5_underflows, s5_errors;

/* stream 0x1821 through a context of ours; with hakios_smmu_lock held */
static bool
s5_space_on(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&s5_smmu, "scr0", SMMU_SCR0), idr0 = get(&s5_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&s5_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff, page, pages;
	uint64_t *root, *second, *third, table_root;

	if (scr0 & 1u << 3) {
		printf("adsp845: %u: this SMMU numbers its streams with 16 bits: not written for\n", step);
		return false;
	}
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	s5_space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(s5_smmu.base + SMMU_SMR(n)), s2cr = rd(s5_smmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ AUDIO_STREAM_6T) & ~mask) == 0) {
				printf("adsp845: %u: match register %u covers stream 0x1821 already (smr %#x, s2cr %#x)\n", step, n, smr, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					s5_space.passed = true;
					return true;
				}
				printf("adsp845: %u: ... and does not pass it untranslated: somebody's, left alone\n", step);
				return false;
			}
		} else if (s5_space.match == ~0u) {
			s5_space.match = n;
			s5_space.old_smr = smr;
			s5_space.old_s2cr = s2cr;
		}
	}
	for (s5_space.context = 0; s5_space.context < contexts && used[s5_space.context]; s5_space.context++) {
	}
	if (s5_space.match == ~0u || s5_space.context >= contexts) {
		printf("adsp845: %u: the SMMU has no match register or no context left\n", step);
		return false;
	}
	s5_smmu_second.phys = s5_smmu.phys + page;
	s5_smmu_context.phys = s5_smmu.phys + (pages + s5_space.context) * page;
	s5_space.old_sctlr = get(&s5_smmu_context, "sctlr", SMMU_CB_SCTLR);
	if (s5_space.old_sctlr & 1) {
		printf("adsp845: %u: context %u is on with no stream through it: somebody's, left alone\n", step, s5_space.context);
		return false;
	}
	if (s5_space.tables == 0 && kmem_alloc(kernel_map, &s5_space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		s5_space.tables = 0;
		return false;
	}
	bzero((void *)s5_space.tables, 3 * 4096);
	root = (uint64_t *)s5_space.tables;
	second = (uint64_t *)(s5_space.tables + 4096);
	third = (uint64_t *)(s5_space.tables + 8192);
	root[AUDIO_ADDRESSES_6T >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[AUDIO_ADDRESSES_6T >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned n = 0; n < S5_BYTES / 4096; n++) {
		third[(AUDIO_ADDRESSES_6T >> 12 & 0x1ff) + n] = kvtophys(s5_buffer + 4096 * n) | AUDIO_PTE;
	}
	for (vm_offset_t line = s5_space.tables; line < s5_space.tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(s5_space.tables) | (uint64_t)s5_space.context << 48;
	printf("adsp845: %u: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets our page tables for %u KB at %#llx, match register %u sends "
	    "stream 0x1821 (mask 0) through it\n", step, s5_space.context, S5_BYTES / 1024, AUDIO_ADDRESSES_6T, s5_space.match);
	put(&s5_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&s5_smmu_second, "cba2r", SMMU_CBA2R(s5_space.context), 1);
	put(&s5_smmu_second, "cbar", SMMU_CBAR(s5_space.context), SMMU_CBAR_STAGE_1);
	put(&s5_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&s5_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&s5_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)table_root);
	put(&s5_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	put(&s5_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&s5_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&s5_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&s5_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&s5_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&s5_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, s5_space.context);
	put(&s5_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(s5_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			return false;
		}
		sleep_ms(1);
	}
	put(&s5_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	s5_space.on = true;
	put(&s5_smmu, "s2cr", SMMU_S2CR(s5_space.match), s5_space.context);
	put(&s5_smmu, "smr", SMMU_SMR(s5_space.match), 1u << 31 | AUDIO_STREAM_6T);
	if (get(&s5_smmu, "smr", SMMU_SMR(s5_space.match)) != (1u << 31 | AUDIO_STREAM_6T)) {
		printf("adsp845: %u: THE SMMU DID NOT TAKE THE MATCH REGISTER\n", step);
		return false;
	}
	printf("adsp845: %u: STREAM 0x1821 GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", step, s5_space.context);
	return true;
}

static void
s5_space_off(void)
{
	uint32_t status, global;

	if (!s5_space.on) {
		return;
	}
	status = rd(s5_smmu_context.base + SMMU_CB_FSR);
	global = rd(s5_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		printf("adsp845: %u: the SMMU refused the DSP nothing (context %u)\n", step, s5_space.context);
	} else {
		printf("adsp845: %u: THE SMMU REFUSED A REQUEST: context %u status %#x, address %#x%08x, syndrome %#x; global %#x stream %#x\n",
		    step, s5_space.context, status, rd(s5_smmu_context.base + SMMU_CB_FAR + 4), rd(s5_smmu_context.base + SMMU_CB_FAR),
		    rd(s5_smmu_context.base + SMMU_CB_FSYNR0), global, rd(s5_smmu.base + SMMU_SGFSYNR1));
	}
	put(&s5_smmu, "smr (given back)", SMMU_SMR(s5_space.match), s5_space.old_smr);
	put(&s5_smmu, "s2cr (given back)", SMMU_S2CR(s5_space.match), s5_space.old_s2cr);
	put(&s5_smmu_context, "sctlr (given back)", SMMU_CB_SCTLR, s5_space.old_sctlr);
	s5_space.on = false;
}

static bool
s5_events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != S_ASM) {
		return false;
	}
	if ((opcode == ASM_DATA_EVENT_WRITE_DONE_V2 || opcode == APR_BASIC_RSP_RESULT) && (token & S5_WRITE_TOKEN)) {
		if ((opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && length >= 16 && payload[3] != 0) || opcode == APR_BASIC_RSP_RESULT) {
			os_atomic_inc(&s5_errors, relaxed);
		}
		os_atomic_inc(&s5_done, relaxed);
		os_atomic_or(&s5_returned, 1u << (token & 31), relaxed);
		return true;
	}
	if (opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		os_atomic_inc(&s5_underflows, relaxed);
		return true;
	}
	return false;
}

/* a command to ASM or ADM: true if answered with status 0, or with `wanted' (then in *reply) */
static bool
s5_command(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct apr_reply *reply)
{
	struct apr_reply local;

	if (reply == NULL) {
		reply = &local;
	}
	printf("adsp845: %u: %s: %s (opcode %#x)\n", step, service == S_ASM ? "ASM" : service == S_ADM ? "ADM" : "AFE", what, opcode);
	snprintf(last_write, sizeof(last_write), "step %u asked: %s", step, what);
	if (!apr_exchange(service, port, 0x15, false, opcode, payload, length, 1000, reply)) {
		printf("adsp845: %u: %s: NO ANSWER\n", step, what);
		return false;
	}
	if (reply->opcode == APR_BASIC_RSP_RESULT && reply->length >= 8 && reply->words[0] == opcode) {
		printf("adsp845: %u: %s: %s (status %#x)\n", step, what, reply->words[1] == 0 ? "done" : "REFUSED", reply->words[1]);
		return reply->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && reply->opcode == wanted) {
		return true;
	}
	printf("adsp845: %u: %s: an answer not expected: opcode %#x, %u bytes, %#x %#x\n", step, what, reply->opcode, reply->length,
	    reply->words[0], reply->words[1]);
	return false;
}

/* 1 kHz at 48 kHz, -40 dBFS (a peak of 328): one cycle (step 7 of the plan starts there too) */
static const int16_t s5_tone[48] = {
	0, 43, 85, 125, 164, 200, 232, 260, 284, 303, 317, 325, 328, 325, 317, 303, 284, 260, 232, 200, 164, 125, 85, 43,
	0, -43, -85, -125, -164, -200, -232, -260, -284, -303, -317, -325, -328, -325, -317, -303, -284, -260, -232, -200, -164, -125,
	-85, -43,
};
/*
 * S8: what Darwin plays (/dev/hakios-audio, from the audio server's plug-in) to the speaker. The sink puts it in a ring; the
 * periods take it from there, the two channels made one (the speaker is one), S8_BELOW_DB under what Darwin wrote and Darwin's own
 * volume under that, a limiter keeping the peaks at -20 dBFS (S5_PEAK_MOST): the left slot, the right one silent, as step 16.
 */
#define S8_RING_FRAMES          16384u          /* 0.34 s */
/*
 * Under what Darwin writes, always. r100: 20 dB made the start's chime -41 dBFS, unheard; r101: 0 dB had YouTube 8x over -20 dBFS,
 * the limiter holding it down all the time, so that Darwin's volume made no difference over most of its range. 12: loud media a
 * little over the limit at Darwin's loudest, Darwin's volume heard over the rest; r104: 12 dB made the keyboard's clicks -47 dBFS,
 * hardly heard. 6: the clicks heard, loud media held by the limiter over the top few steps of Darwin's volume.
 */
#define S8_BELOW_DB             6u
#define S8_SILENCE              8               /* a sample's size under which it is taken for silence */
#define S8_QUIET_US             3000000ull      /* this long without sound: the amplifier off */
static int16_t s8_ring[2 * S8_RING_FRAMES];
static volatile uint32_t s8_head, s8_tail, s8_peak_in, s8_peak_out, s8_gain_q15;
static volatile uint64_t s8_last_sound_us, s8_frames_in, s8_frames_fed, s8_frames_silent;
static volatile bool s8_mode, s8_stopping, s8_running;
static uint32_t s8_limit_q15 = 32768;           /* the limiter's gain: down at once to keep a period's peak at -20 dBFS, back up slowly */
static volatile uint32_t s8_limited_periods;

/* 10^(-dB/20) in Q15, 0..96 dB */
static const uint16_t s8_db_q15[97] = {
	32767, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362, 9235, 8231, 7336, 6538, 5827, 5193, 4629, 4125,
	3677, 3277, 2920, 2603, 2320, 2068, 1843, 1642, 1464, 1305, 1163, 1036, 924, 823, 734, 654, 583, 519, 463, 413, 368, 328, 292,
	260, 232, 207, 184, 164, 146, 130, 116, 104, 92, 82, 73, 65, 58, 52, 46, 41, 37, 33, 29, 26, 23, 21, 18, 16, 15, 13, 12, 10, 9,
	8, 7, 7, 6, 5, 5, 4, 4, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

/* Darwin's volume (eighths of a dB under its loudest) and S8_BELOW_DB: the gain, Q15 */
static uint32_t
s8_gain_for(uint32_t below_eighths)
{
	uint32_t db = S8_BELOW_DB + (below_eighths + 4) / 8;

	return db > 96 ? 0 : s8_db_q15[db];
}

/* From the writer's thread (the audio server's real-time one): never waits; what does not fit is dropped */
static uint32_t
s8_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = s8_head, room = S8_RING_FRAMES - (head - s8_tail), taken = count < room ? count : room, peak = s8_peak_in;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t left = frames[2 * i], right = frames[2 * i + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right : (uint32_t)right;

		s8_ring[2 * ((head + i) % S8_RING_FRAMES)] = left;
		s8_ring[2 * ((head + i) % S8_RING_FRAMES) + 1] = right;
		sound |= l > S8_SILENCE || r > S8_SILENCE;
		peak = l > peak ? l : r > peak ? r : peak;
	}
	OSMemoryBarrier();
	s8_head = head + taken;
	s8_frames_in += taken;
	s8_peak_in = peak;
	if (sound) {
		s8_last_sound_us = now_us();
	}
	return taken;
}

static uint32_t s5_tone_at;
static volatile uint32_t s5_gain;               /* the tone's: x1 -40 dBFS, x2 -34, x4 -28, x8 -22; 0 silence */
static bool s5_left_only;
#define S5_PEAK_MOST            3277            /* -20 dBFS: never more (the plan's section 4.2), whatever the gain asks */
_Static_assert(328 * 8 <= S5_PEAK_MOST, "the loudest step of the tone is over -20 dBFS");

static void
s5_period_fill(unsigned period)
{
	int16_t *samples = (int16_t *)(s5_buffer + 4096 * period);
	uint32_t gain = s5_gain > 8 ? 8 : s5_gain;

	if (s8_mode) {
		uint32_t tail = s8_tail, available = s8_head - tail, n = available < S5_PERIOD_FRAMES ? available : S5_PERIOD_FRAMES;
		int32_t q15 = (int32_t)s8_gain_q15, peak = (int32_t)s8_peak_out, most = 0;

		OSMemoryBarrier();
		/* the limiter: the period's peak after the gain, and the gain that keeps it at S5_PEAK_MOST (the clamp below only catches rounding) */
		for (uint32_t i = 0; i < n; i++) {
			int32_t one = ((int32_t)s8_ring[2 * ((tail + i) % S8_RING_FRAMES)] + s8_ring[2 * ((tail + i) % S8_RING_FRAMES) + 1]) / 2;
			int32_t v = one * q15 >> 15;

			most = v > most ? v : -v > most ? -v : most;
		}
		s8_limit_q15 += (32768 - s8_limit_q15) / 16;    /* back up: about 0.3 s */
		if ((int64_t)most * s8_limit_q15 >> 15 > S5_PEAK_MOST) {
			s8_limit_q15 = (uint32_t)((int64_t)S5_PEAK_MOST * 32768 / most);
			s8_limited_periods++;
		}
		q15 = (int32_t)((int64_t)q15 * s8_limit_q15 >> 15);
		for (uint32_t i = 0; i < n; i++) {
			int32_t one = ((int32_t)s8_ring[2 * ((tail + i) % S8_RING_FRAMES)] + s8_ring[2 * ((tail + i) % S8_RING_FRAMES) + 1]) / 2;
			int32_t v = one * q15 >> 15;

			v = v > S5_PEAK_MOST ? S5_PEAK_MOST : v < -S5_PEAK_MOST ? -S5_PEAK_MOST : v;
			samples[2 * i] = (int16_t)v;
			samples[2 * i + 1] = 0;
			peak = v > peak ? v : -v > peak ? -v : peak;
		}
		bzero(samples + 2 * n, (S5_PERIOD_FRAMES - n) * 4);
		OSMemoryBarrier();
		s8_tail = tail + n;
		s8_peak_out = (uint32_t)peak;
		s8_frames_fed += n;
		s8_frames_silent += S5_PERIOD_FRAMES - n;
	} else for (unsigned frame = 0; frame < S5_PERIOD_FRAMES; frame++) {
		int32_t sample = (int32_t)s5_tone[s5_tone_at++ % 48] * (int32_t)gain;

		sample = sample > S5_PEAK_MOST ? S5_PEAK_MOST : sample < -S5_PEAK_MOST ? -S5_PEAK_MOST : sample;
		samples[2 * frame] = (int16_t)sample;
		samples[2 * frame + 1] = s5_left_only ? 0 : (int16_t)sample;
	}
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + S5_PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

static bool
s5_period_write(unsigned period, uint32_t handle)
{
	uint32_t write[8] = { (uint32_t)(s5_address + 4096 * period), (uint32_t)((s5_address + 4096 * period) >> 32), handle, S5_PERIOD_BYTES,
		              period, 0, 0, 0 };

	return apr_exchange(S_ASM, S5_STREAM_PORT, S5_WRITE_TOKEN | period, false, ASM_DATA_CMD_WRITE_V2, write, sizeof(write), 0, NULL);
}

/*
 * The amplifier (S6/S7, lane-b-notes/tfa9894-s6-config.md, from NXP's tfa98xx of OnePlus's kernel, N1 fields of rev 0x1a94): bypass, no
 * CoolFlux. Every write read back. Its status, reg 0x10: VDDS b0, PLLS b1, OTDS b2, OVDS b3, UVDS b4 (these three: 1 = all is well),
 * OCDS b5 (an over-current, sticky: read once), CLKS b6, NOCLK b8, WDS b10, AMPS b12, AREFS b13, BODNOK b15; reg 0x11: CLKOOR b8,
 * MANALARM b9, TDMERR b10.
 */
static struct bitbang s5_bus;
static bool s5_amp_on;
static uint32_t s5_found_ctl[2], s5_found_io[2];  /* gpio89 and 90 as found, given back at the end */
static bool s5_took_bus;

static bool
tfa_write(struct bitbang *b, uint8_t reg, uint16_t value)
{
	bool ok;
	uint16_t back = 0;

	bb_start(b);
	ok = bb_write(b, TFA_ADDRESS << 1) && bb_write(b, reg) && bb_write(b, (uint8_t)(value >> 8)) && bb_write(b, (uint8_t)value);
	bb_stop(b);
	if (ok) {
		ok = tfa_read(b, reg, &back) && back == value;
	}
	printf("adsp845: %u: amplifier reg %#04x <- %#06x: %s (read back %#06x)\n", step, reg, value, ok ? "done" : "NOT TAKEN", back);
	return ok;
}

/* a fault in the status: what, or NULL */
static const char *
tfa_fault(uint16_t s10, uint16_t s11, bool amp)
{
	if (!(s10 & 0x0004)) return "over temperature (OTDS 0)";
	if (!(s10 & 0x0008)) return "over voltage (OVDS 0)";
	if (!(s10 & 0x0010)) return "under voltage (UVDS 0)";
	if (s10 & 0x0020) return "over current (OCDS)";
	if (s10 & 0x0100) return "the clock lost (NOCLK)";
	if (s10 & 0x0400) return "the watchdog (WDS)";
	if (s10 & 0x8000) return "brown-out (BODNOK)";
	if (!(s10 & 0x0002) || !(s10 & 0x0040)) return "the PLL or the clock not stable (PLLS, CLKS)";
	if (amp && (!(s10 & 0x1000) || !(s10 & 0x2000))) return "the amplifier or its reference not on (AMPS, AREFS)";
	if (s11 & 0x0700) return "a clock out of range, the manager's alarm or a TDM error (0x11)";
	return NULL;
}

static void
tfa_down(void)
{
	uint16_t s10 = 0;

	if (!s5_amp_on) {
		return;
	}
	printf("adsp845: %u: the amplifier off: AMPE 0, 10 ms, power down\n", step);
	tfa_write(&s5_bus, 0x00, 0x8220);
	delay(10000);
	tfa_write(&s5_bus, 0x00, 0x8201);
	tfa_read(&s5_bus, 0x10, &s10);
	printf("adsp845: %u: the amplifier's status after: %#06x\n", step, s10);
	s5_amp_on = false;
}

static bool
tfa_up(void)
{
	uint16_t s10 = 0, s11 = 0, s14 = 0, v = 0;
	const char *fault;

	/*
	 * A TFA9894, powered down: as step 13 found it (0x8245, its value after a reset) or as tfa_down() leaves it (0x8201). (Reg 0x01
	 * is not looked at: its bit 3 is the chip's own -- 0x11cc written reads 0x11c4 once it has been on and off, r99 -- and it is
	 * written again below.)
	 */
	if (!tfa_read(&s5_bus, 0x03, &v) || v != 0x1a94 || !tfa_read(&s5_bus, 0x00, &v) || (v != 0x8245 && v != 0x8201)) {
		printf("adsp845: %u: the amplifier is not a TFA9894 powered down (reg %#06x): nothing written\n", step, v);
		return false;
	}
	s5_amp_on = true;               /* (from here on, tfa_down() puts it back to power-down) */
	if (!tfa_write(&s5_bus, 0x00, 0x8201) || !tfa_write(&s5_bus, 0x01, 0x11cc) || !tfa_write(&s5_bus, 0x50, 0x1391) ||
	    !tfa_write(&s5_bus, 0x00, 0x8200)) {
		return false;
	}
	for (unsigned ms = 0; ms <= 100; ms += 5) {
		delay(5000);
		tfa_read(&s5_bus, 0x10, &s10);
		if ((s10 & 0x0042) == 0x0042) {
			break;
		}
	}
	tfa_read(&s5_bus, 0x11, &s11);
	tfa_read(&s5_bus, 0x14, &s14);
	printf("adsp845: %u: the amplifier out of power-down: status %#06x %#06x, manager state %u\n", step, s10, s11, (s14 >> 3) & 0xf);
	if ((fault = tfa_fault(s10, s11, false)) != NULL) {
		printf("adsp845: %u: THE AMPLIFIER IS NOT WELL: %s: it stays off\n", step, fault);
		return false;
	}
	if (!tfa_write(&s5_bus, 0x00, 0x8220)) {
		return false;
	}
	delay(10000);
	if (!tfa_write(&s5_bus, 0x00, 0x8228)) {
		return false;
	}
	delay(10000);
	tfa_read(&s5_bus, 0x10, &s10);
	tfa_read(&s5_bus, 0x11, &s11);
	tfa_read(&s5_bus, 0x14, &s14);
	if ((fault = tfa_fault(s10, s11, true)) != NULL) {
		printf("adsp845: %u: THE AMPLIFIER DID NOT COME ON WELL: %s (status %#06x %#06x, manager state %u)\n", step, fault, s10, s11,
		    (s14 >> 3) & 0xf);
		return false;
	}
	printf("adsp845: %u: THE AMPLIFIER IS ON (bypass, the DSP's samples straight in): status %#06x %#06x, manager state %u\n", step, s10,
	    s11, (s14 >> 3) & 0xf);
	return true;
}

static bool stream_play(unsigned mode);       /* 0: the tone, the amplifier off (S5); 1: the tone's ladder, heard (S7); 2: Darwin's (S8) */

static bool
step_stream(void)
{
	return stream_play(0);
}

/* S6 and S7: the amplifier on, and a tone it plays: the left channel, 1 kHz, -40, -34, -28, -22 dBFS, two seconds each */
static bool
step_speaker(void)
{
	if (last_step >= 17) {
		printf("adsp845: %u: the tone's ladder is not played: step 17 follows, and puts the amplifier on itself\n", step);
		return true;
	}
	return stream_play(1);
}

/* the gpio89/90 bus taken for the amplifier, as found kept */
static void
s5_take_bus(void)
{
	s5_found_ctl[0] = get(&pins[5].page, "gpio89 control", 0);
	s5_found_io[0] = get(&pins[5].page, "gpio89 I/O", 4);
	s5_found_ctl[1] = get(&pins[6].page, "gpio90 control", 0);
	s5_found_io[1] = get(&pins[6].page, "gpio90 I/O", 4);
	s5_bus.sda = &pins[5];
	s5_bus.scl = &pins[6];
	s5_took_bus = true;
	printf("adsp845: %u: about to make gpio89/90 the amplifier's I2C by hand (were %#x %#x)\n", step, s5_found_ctl[0], s5_found_ctl[1]);
	bb_line(&pins[5], true);
	bb_line(&pins[6], true);
}

/*
 * S8: Darwin's sound to the speaker until the run ends (hakios_adsp_stop()). The amplifier comes on when sound comes (and stays off
 * for the rest of the run after a fault), goes off after 3 s of silence; its status is read every 200 ms while it is on.
 */
static bool
s8_loop(uint32_t handle, uint32_t *sent_out)
{
	uint32_t sent = 0, failed_writes = 0, ups = 0, looks = 0, below = 0, generation = 0, done_seen = s5_done, said_gen = ~0u;
	uint64_t last_look = 0, last_say = now_us(), last_done = now_us(), began = now_us(), frames_said = 0;
	const char *fault = NULL;
	bool muted = false, failed = false;

	s8_head = s8_tail = 0;
	s8_peak_in = s8_peak_out = 0;
	s8_last_sound_us = 0;
	s8_frames_in = s8_frames_fed = s8_frames_silent = 0;
	s8_gain_q15 = 0;
	s8_mode = true;
	s5_returned = 0;
	hakios_adsp_apr_events(s5_events);
	for (unsigned n = 0; n < S5_PERIODS; n++) {
		s5_period_fill(n);
		if (!s5_period_write(n, handle)) {
			printf("adsp845: %u: the write of period %u could not be sent\n", step, n);
			failed = true;
			break;
		}
		sent++;
	}
	s5_take_bus();
	hakios_audio_set_sink("the 6T's speaker (adsp845, step 17)", s8_sink);
	s8_limit_q15 = 32768;
	s8_limited_periods = 0;
	tell("adsp845: %u: DARWIN'S SOUND GOES TO THE SPEAKER now, until the run ends: %u dB under what it writes and its volume under that, "
	    "a limiter keeping it under -20 dBFS; the amplifier on while there is sound\n", step, S8_BELOW_DB);
	while (!failed && !s8_stopping) {
		uint64_t now = now_us();
		bool sound = s8_last_sound_us != 0 && now - s8_last_sound_us < S8_QUIET_US;
		uint32_t back = s5_returned;

		if (!hakios_audio_volume(&below, &muted, &generation)) {
			below = 0;
			muted = false;
		}
		if (generation != said_gen) {
			said_gen = generation;
			tell("adsp845: %u: Darwin's volume: %u.%u dB under its loudest%s (setting %u)\n", step, below / 8, below % 8 * 10 / 8,
			    muted ? ", MUTED" : "", generation);
		}
		if (sound && !s5_amp_on && fault == NULL) {
			tell("adsp845: %u: sound from Darwin (its peak %u): the amplifier on\n", step, s8_peak_in);
			if (tfa_up()) {
				ups++;
			} else {
				tfa_down();
				fault = "it did not come on well";
				tell("adsp845: %u: THE AMPLIFIER STAYS OFF for the rest of the run\n", step);
			}
			last_look = now_us();
		} else if (!sound && s5_amp_on) {
			s8_gain_q15 = 0;
			tell("adsp845: %u: 3 s of silence: the amplifier off\n", step);
			tfa_down();
		}
		s8_gain_q15 = s5_amp_on && !muted ? s8_gain_for(below) : 0;
		if (s5_amp_on && now - last_look >= 200000) {
			uint16_t s10 = 0, s11 = 0;

			last_look = now;
			looks++;
			tfa_read(&s5_bus, 0x10, &s10);
			tfa_read(&s5_bus, 0x11, &s11);
			if ((fault = tfa_fault(s10, s11, true)) != NULL) {
				s8_gain_q15 = 0;
				tell("adsp845: %u: THE AMPLIFIER REPORTS %s (status %#06x %#06x): OFF NOW, and for the rest of the run\n", step, fault,
				    s10, s11);
				tfa_down();
			}
		}
		if (back != 0) {
			os_atomic_andnot(&s5_returned, back, relaxed);
			for (unsigned n = 0; n < S5_PERIODS; n++) {
				if (back & 1u << n) {
					s5_period_fill(n);
					if (s5_period_write(n, handle)) {
						sent++;
					} else {
						failed_writes++;
					}
				}
			}
		}
		if (s5_done != done_seen) {
			done_seen = s5_done;
			last_done = now;
		} else if (now - last_done > 1000000ull) {
			tell("adsp845: %u: THE DSP HAS GIVEN NO PERIOD BACK FOR 1 s (%u of %u): Darwin's sound stops here\n", step, s5_done, sent);
			failed = true;
		}
		if (now - last_say >= (sound ? 2000000ull : 10000000ull) && s8_frames_in != frames_said) {
			last_say = now;
			frames_said = s8_frames_in;
			tell("adsp845: %u: from Darwin %llu frames (its peak %u), to the speaker %llu (their peak %u of 3277; %u periods held down "
			    "by the limiter), %llu of silence filled in; %u underflows; the amplifier %s, on %u times, looked at %u times\n", step,
			    s8_frames_in, s8_peak_in, s8_frames_fed, s8_peak_out, s8_limited_periods, s8_frames_silent, s5_underflows,
			    s5_amp_on ? "ON" : "off", ups, looks);
			s8_peak_in = s8_peak_out = 0;
		}
		sleep_ms(2);
	}
	hakios_audio_set_sink("nothing", NULL);
	s8_gain_q15 = 0;
	tfa_down();
	sleep_ms(200);                          /* (the periods in flight, silence now, played out) */
	hakios_adsp_apr_events(NULL);
	s8_mode = false;
	tell("adsp845: %u: Darwin's sound to the speaker over after %llu s: from Darwin %llu frames, to the speaker %llu, %llu of silence; "
	    "%u periods written (%u could not be), %u underflows, %u with an error; the amplifier on %u times%s%s\n", step,
	    (now_us() - began) / 1000000, s8_frames_in, s8_frames_fed, s8_frames_silent, sent, failed_writes, s5_underflows, s5_errors, ups,
	    fault ? "; it reported " : "", fault ? fault : "");
	*sent_out = sent;
	return !failed && fault == NULL;
}

static bool
step_darwin(void)
{
	bool ok;

	s8_running = true;
	ok = stream_play(2);
	s8_running = false;
	return ok;
}

static bool
stream_play(unsigned mode)
{
	bool amp = mode != 0;
	struct afe_i2s_config i2s = { 1, MI2S_BITS, 2, 1, 1, MI2S_RATE, 0, 0 };
	uint16_t port[2] = { QUAT_MI2S_RX, 0 };
	uint32_t found_ctl[4], found_io[4], handle = 0, copp = 0, sent = 0;
	bool clock = false, started = false, mapped = false, copp_open = false, stream_open = false, ok = false, smmu = false;
	struct apr_reply reply;

	if (mode == 2) {
		announce("as step 16, and then DARWIN'S SOUND (/dev/hakios-audio) to the speaker until the run ends: one channel of the two "
		    "channels' mean at Darwin's volume, a limiter keeping it under -20 dBFS; the amplifier on while there is sound "
		    "(off after 3 s of silence, and for good after a fault), its status read every 200 ms. AUDIBLE");
	} else if (amp) {
		announce("as step 15, and then the AMPLIFIER: bypass, out of power-down, on (every write read back); a 1 kHz tone on the left "
		    "channel at -40, -34, -28 and -22 dBFS, 2 s each, its status read every 200 ms (any fault: off at once); the amplifier off "
		    "before anything else is stopped. AUDIBLE");
	} else {
		announce("the port of step 14 started again; WRITE the SMMU (stream 0x1821 through a context of ours); ASK the DSP for our memory "
		    "(ASM), a device path to port 0x1006 (ADM, 16 bits) and a stream routed to it; WRITE a 1 kHz tone at -40 dBFS into it for 2 s, "
		    "the amplifier off; close, stop, give back");
	}
	if (s5_buffer == 0 && kmem_alloc(kernel_map, &s5_buffer, S5_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		s5_buffer = 0;
		printf("adsp845: %u: no memory for the periods\n", step);
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		found_ctl[i] = get(&pins[i].page, "control", 0);
		found_io[i] = get(&pins[i].page, "I/O", 4);
	}
	lck_mtx_lock(&hakios_smmu_lock);
	smmu = s5_space_on();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!smmu) {
		goto out;
	}
	s5_address = s5_space.passed ? (kvtophys(s5_buffer) | AUDIO_STREAM_BITS_6T) : (AUDIO_ADDRESSES_6T | AUDIO_STREAM_BITS_6T);
	clock = afe_mi2s_clock(true);
	if (!clock || !afe_param_send("the port 0x1006: I2S, 16 bits, stereo on SD1, 48 kHz", true, AFE_MODULE_AUDIO_DEV_INTERFACE,
	    AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s))) {
		goto out;
	}
	for (unsigned i = 0; i < 4; i++) {
		bool driven = i < 2;

		if (driven) {
			put(&pins[i].page, "I/O", 4, 2);
		}
		put(&pins[i].page, "control", 0, 1u << 2 | 3u << 6 | (driven ? 1u << 9 : 0));
	}
	started = afe_command("the port 0x1006, started", AFE_PORT_CMD_DEVICE_START, port, sizeof(port));
	if (!started) {
		goto out;
	}
	{
		struct {
			uint16_t pool, regions;
			uint32_t property, low, high, size;
		} __attribute__((packed)) map = { 3, 1, 0, (uint32_t)s5_address, (uint32_t)(s5_address >> 32), S5_BYTES };
		struct {
			uint16_t flags, mode, endpoint1, endpoint2;
			uint32_t topology;
			uint16_t channels, bits;
			uint32_t rate;
			uint8_t map[8];
		} __attribute__((packed)) open = { 0, 1, QUAT_MI2S_RX, 0xffff, ADM_TOPOLOGY_NONE, 2, 16, MI2S_RATE, { 1, 2 } };
		struct {
			uint32_t matrix, sessions;
			uint16_t session, copps;
			uint16_t copp[2];
		} __attribute__((packed)) route = { 0, 1, 1, 1, { 0, 0 } };
		struct {
			uint32_t flags;
			uint16_t sink, bits;
			uint32_t topology, format;
		} __attribute__((packed)) write = { 0, 0, 16, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
		struct {
			uint32_t size;
			uint16_t channels, bits;
			uint32_t rate;
			uint16_t is_signed, reserved;
			uint8_t map[8];
		} __attribute__((packed)) format = { 20, 2, 16, MI2S_RATE, 1, 0, { 1, 2 } };
		uint32_t run[3] = { 0, 0, 0 };

		if (!s5_command("our periods, mapped for the DSP", S_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
		    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &reply)) {
			goto out;
		}
		mapped = true;
		handle = reply.words[0];
		printf("adsp845: %u: our %u KB at %#llx are the DSP's handle %#x\n", step, S5_BYTES / 1024, s5_address, handle);
		if (!s5_command("a device path to port 0x1006: playback, no processing, 2 channels of 16 bits at 48 kHz", S_ADM, QUAT_MI2S_RX,
		    ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5, &reply)) {
			goto out;
		}
		if (reply.words[0] != 0) {
			printf("adsp845: %u: the device path: REFUSED (status %#x)\n", step, reply.words[0]);
			goto out;
		}
		copp_open = true;
		copp = reply.words[1] & 0xffff;
		route.copp[0] = (uint16_t)copp;
		printf("adsp845: %u: THE DEVICE PATH IS OPEN: COPP %u\n", step, copp);
		if (!s5_command("the stream, opened for writing: 16-bit linear PCM, no processing", S_ASM, S5_STREAM_PORT,
		    ASM_STREAM_CMD_OPEN_WRITE_V3, &write, sizeof(write), 0, NULL)) {
			goto out;
		}
		stream_open = true;
		if (!s5_command("session 1 routed to the device path", S_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route), 0, NULL) ||
		    !s5_command("the stream's format: 2 x 16 bits at 48 kHz", S_ASM, S5_STREAM_PORT, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &format,
		    sizeof(format), 0, NULL) ||
		    !s5_command("the session, run", S_ASM, S5_STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0, NULL)) {
			goto out;
		}
	}
	if (mode == 2) {
		ok = s8_loop(handle, &sent);
	} else {
		uint64_t began = now_us();
		uint32_t done_before, toggles_tone = 0, ones = 0;
		bool tone_over = false, failed = false;

		uint64_t last_look = 0;
		unsigned looks = 0, rung_at = 0;
		const char *amp_fault = NULL;

		hakios_adsp_apr_events(s5_events);
		s5_returned = 0;
		s5_tone_at = 0;
		s5_left_only = amp;
		s5_gain = amp ? 0 : 1;          /* (with the amplifier: silence until it is on) */
		done_before = s5_done;
		for (unsigned n = 0; n < S5_PERIODS; n++) {
			s5_period_fill(n);
			if (!s5_period_write(n, handle)) {
				printf("adsp845: %u: the write of period %u could not be sent\n", step, n);
				failed = true;
				break;
			}
			sent++;
		}
		if (amp) {
			s5_take_bus();
			if (!tfa_up()) {
				tfa_down();
				failed = true;
			} else {
				s5_gain = 1;
				began = now_us();
				printf("adsp845: %u: THE TONE: left channel, 1 kHz, -40 dBFS\n", step);
			}
		}
		while (!failed) {
			uint32_t back = s5_returned;
			uint64_t now = now_us();

			if (amp && s5_amp_on && now - last_look >= 200000) {
				uint16_t s10 = 0, s11 = 0;

				last_look = now;
				looks++;
				tfa_read(&s5_bus, 0x10, &s10);
				tfa_read(&s5_bus, 0x11, &s11);
				if ((amp_fault = tfa_fault(s10, s11, true)) != NULL) {
					printf("adsp845: %u: THE AMPLIFIER REPORTS %s (status %#06x %#06x): OFF NOW\n", step, amp_fault, s10, s11);
					s5_gain = 0;
					tfa_down();
					tone_over = true;
				} else if (looks % 5 == 0) {
					printf("adsp845: %u: the amplifier's status %#06x %#06x (look %u)\n", step, s10, s11, looks);
				}
			}
			if (amp && !tone_over && now - began >= 2000000ull * (rung_at + 1) && rung_at < 3) {
				rung_at++;
				s5_gain = 1u << rung_at;
				printf("adsp845: %u: THE TONE: %d dBFS\n", step, -40 + 6 * (int)rung_at);
			}

			if (back != 0) {
				os_atomic_andnot(&s5_returned, back, relaxed);
				for (unsigned n = 0; n < S5_PERIODS && !tone_over; n++) {
					if (back & 1u << n) {
						s5_period_fill(n);
						if (!s5_period_write(n, handle)) {
							tone_over = true;
							break;
						}
						sent++;
					}
				}
			}
			if (toggles_tone == 0 && now - began > 500000) {
				toggles_tone = pin_toggles(&pins[3], &ones);    /* gpio61, the data, while the tone plays */
			}
			if (!tone_over && now - began >= (amp ? 8000000ull : 2000000ull)) {
				tone_over = true;
				if (amp) {
					s5_gain = 0;            /* silence while the last periods drain */
				}
			}
			if (tone_over && s5_done - done_before >= sent) {
				break;
			}
			if (now - began > (amp ? 10000000ull : 4000000ull) || (s5_done == done_before && now - began > 1000000ull)) {
				printf("adsp845: %u: THE DSP HAS NOT GIVEN THE PERIODS BACK (%u of %u)\n", step, s5_done - done_before, sent);
				failed = true;
				break;
			}
			sleep_ms(2);
		}
		hakios_adsp_apr_events(NULL);
		if (amp) {
			tfa_down();                     /* before the stream, the port and the clock are stopped */
			printf("adsp845: %u: the amplifier's status looked at %u times while it played%s%s\n", step, looks,
			    amp_fault ? "; it reported " : "", amp_fault ? amp_fault : "");
		}
		{
			uint64_t us = now_us() - began, frames = (uint64_t)(s5_done - done_before) * S5_PERIOD_FRAMES;

			printf("adsp845: %u: %u periods written, %u given back (%u with an error), %u underflows, in %llu ms: the DSP took %llu "
			    "frames a second; the data pin (gpio61) %u changes while the tone played (the amplifier off: nothing to hear)\n", step,
			    sent, s5_done - done_before, s5_errors, s5_underflows, us / 1000, us ? frames * 1000000 / us : 0, toggles_tone);
		}
		ok = !failed && s5_errors == 0 && s5_done - done_before >= sent && sent > S5_PERIODS && amp_fault == NULL;
		printf("adsp845: %u: %s\n", step, ok ? "THE STREAM PLAYS: the DSP takes our samples through the SMMU at the port's pace" :
		    "THE STREAM DID NOT PLAY CLEANLY");
	}
out:
	if (amp && s5_took_bus) {
		tfa_down();
		put(&pins[5].page, "gpio89 I/O (given back)", 4, s5_found_io[0] & 2);
		put(&pins[5].page, "gpio89 control (given back)", 0, s5_found_ctl[0]);
		put(&pins[6].page, "gpio90 I/O (given back)", 4, s5_found_io[1] & 2);
		put(&pins[6].page, "gpio90 control (given back)", 0, s5_found_ctl[1]);
		s5_took_bus = false;
	}
	if (stream_open) {
		s5_command("the stream, closed", S_ASM, S5_STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0, NULL);
	}
	if (copp_open) {
		s5_command("the device path, closed", S_ADM, (uint16_t)copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0, NULL);
	}
	if (mapped) {
		s5_command("our periods, unmapped", S_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle, sizeof(handle), 0, NULL);
	}
	if (started) {
		afe_command("the port 0x1006, stopped", AFE_PORT_CMD_DEVICE_STOP, port, sizeof(port));
	}
	if (clock) {
		afe_mi2s_clock(false);
	}
	lck_mtx_lock(&hakios_smmu_lock);
	s5_space_off();
	lck_mtx_unlock(&hakios_smmu_lock);
	for (unsigned i = 0; i < 4; i++) {
		put(&pins[i].page, "I/O (given back)", 4, found_io[i] & 2);
		put(&pins[i].page, "control (given back)", 0, found_ctl[i]);
	}
	return ok;
}

/* ---- the microphone: steps 21 to 28 (M1..M8 of lane-b-notes/mic-shortest-path.md) -------------------------------------- */
/*
 * The 6T's microphones are the WCD9340's (Tavil): its analog inputs, its ADCs and decimators, and its SLIMbus port to the audio
 * DSP's AFE (SLIMBUS_0_TX, 0x4001). The application processor talks to the codec only over SLIMbus, through LPASS's NGD (a
 * satellite of the DSP, which is the bus's master) and its BAM, and only once the DSP has powered them up when asked over QMI
 * (service 0x301). Agreed to by the user, 11-14: the codec's writes (its reset, MIC BIAS 2.7 V), the SLIMbus pins, the NGD's and
 * the BAM's registers, SMMU stream 0x1806, the RPMh vote for its MCLK (LN_BB_CLK2), the DSP's SLIMbus and capture commands. No PMIC
 * write is needed (the codec's supplies are S4A, 1.8 V, always on). Nothing is played. Steps 21 and up do not run 12 to 20.
 */
#define MIC_STREAM_6T           0x1806u         /* sdm845.dtsi: the NGD's iommus */
#define MIC_FIRST_STEP          21u

static struct pin_look mic_pins[] = {
	{ { "gpio54", TLMM_NORTH + 0x1000u * 54, 0x1000, 0 }, 54, 0, "the codec's interrupt (wcd_intr, not used: polled)" },
	{ { "gpio64", TLMM_NORTH + 0x1000u * 64, 0x1000, 0 }, 64, 0, "the codec's reset (high: running)" },
	{ { "gpio70", TLMM_EAST + 0x1000u * 70, 0x1000, 0 }, 70, 1, "SLIMbus clock (lpass_slimbus)" },
	{ { "gpio71", TLMM_EAST + 0x1000u * 71, 0x1000, 0 }, 71, 1, "SLIMbus data 0 (lpass_slimbus)" },
	{ { "gpio72", TLMM_EAST + 0x1000u * 72, 0x1000, 0 }, 72, 1, "SLIMbus data 1 (lpass_slimbus)" },
};

/* an RPMh resource's address in the boot loader's command database (cmd-db), or 0 */
static uint32_t
cmd_db_address(const char *name)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* every resource of cmd-db whose name has "clk" or "bb" in it (the codec's MCLK is one: lnbclka2 is not there on the 6T, r111) */
static void
cmd_db_clocks(void)
{
	char line[200];
	int at = 0;

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			char id[9] = { 0 };

			memcpy(id, entry[i].id, 8);
			if (strnstr(id, "clk", sizeof(id)) == NULL && strnstr(id, "bb", sizeof(id)) == NULL) {
				continue;
			}
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s@%#x", id, entry[i].address);
			if (at > 150) {
				printf("adsp845: %u: cmd-db clocks (slave %u):%s\n", step, slave->id, line);
				at = 0;
			}
		}
		if (at > 0) {
			printf("adsp845: %u: cmd-db clocks (slave %u):%s\n", step, slave->id, line);
			at = 0;
		}
	}
}

/* what the SMMU does with a stream now: read only */
static void
smmu_stream_look(uint32_t stream)
{
	uint32_t idr0 = get(&s5_smmu, "idr0", SMMU_IDR0), matches = idr0 & 0xff, found = 0;

	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(s5_smmu.base + SMMU_SMR(n)), s2cr = rd(s5_smmu.base + SMMU_S2CR(n));
		uint32_t id = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31 && ((id ^ stream) & ~mask) == 0) {
			static const char *const kinds[] = { "translated (a context)", "PASSED untranslated", "FAULTED", "reserved" };

			printf("adsp845: %u: SMMU stream %#x: match register %u (smr %#x, s2cr %#x): %s, context %u\n", step, stream, n, smr, s2cr,
			    kinds[s2cr >> 16 & 3], s2cr & 0xff);
			found++;
		}
	}
	if (found == 0) {
		printf("adsp845: %u: SMMU stream %#x: no match register covers it (the SMMU's default for unmatched streams applies)\n", step,
		    stream);
	}
}

/* M1: what is there, read only. The NGD and the BAM are not read: before the DSP powers them up (QMI), a read can hang the bus. */
static bool
step_mic_survey(void)
{
	static const char *const names[] = { "lnbclka2", "lnbclkg2", "lnbclka3", "rfclka2" };

	announce("READ only, for the microphone: cmd-db's LN_BB_CLK2 (the codec's MCLK), gpio54/64/70-72 (the codec's interrupt and "
	    "reset, SLIMbus), the DSP's QRTR services 0x301 (SLIMbus) and 0x42 (the audio PD), SMMU stream 0x1806 (the NGD's); NOT the NGD "
	    "nor the BAM");
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t address = cmd_db_address(names[i]);

		printf("adsp845: %u: cmd-db \"%s\": %s%#x\n", step, names[i], address ? "at " : "NOT THERE ", address);
	}
	for (unsigned i = 0; i < sizeof(mic_pins) / sizeof(mic_pins[0]); i++) {
		uint32_t ctl = get(&mic_pins[i].page, "control", 0), io = get(&mic_pins[i].page, "I/O", 4);

		printf("adsp845: %u: gpio%u (%s): control %#x (function %u%s, %s, pull %u), I/O %#x (reads %u, would drive %u)\n", step,
		    mic_pins[i].number, mic_pins[i].what, ctl, (ctl >> 2) & 0xf, ((ctl >> 2) & 0xf) == mic_pins[i].function ? ", as wanted" :
		    ", NOT as wanted", (ctl & (1u << 9)) ? "an output" : "an input", ctl & 3, io, io & 1, (io >> 1) & 1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && (qrtr_servers[i].service == 0x301 || qrtr_servers[i].service == 0x42)) {
			printf("adsp845: %u: QRTR service %#x instance %u at node %u port %u\n", step, qrtr_servers[i].service,
			    qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port);
		}
	}
	cmd_db_clocks();
	smmu_stream_look(MIC_STREAM_6T);
	smmu_stream_look(AUDIO_STREAM_6T);
	printf("adsp845: %u: over; nothing was written\n", step);
	return true;
}

/*
 * QMI: a request (header: type 0, transaction, message id, length; then TLVs: type, u16 length, value) to the service's node and
 * port; the answer's TLV 2 is {u16 result, u16 error}, result 0 when done. Returns the result, or -1 without an answer.
 */
static uint16_t qmi_txn = 1;

static int
qmi_ask(uint32_t node, uint32_t port, uint16_t message, const uint8_t *tlvs, uint16_t tlv_length, const char *what)
{
	uint8_t request[64];
	uint16_t txn = qmi_txn++;

	if (7u + tlv_length > sizeof(request)) {
		return -1;
	}
	request[0] = 0;
	request[1] = (uint8_t)txn;
	request[2] = (uint8_t)(txn >> 8);
	request[3] = (uint8_t)message;
	request[4] = (uint8_t)(message >> 8);
	request[5] = (uint8_t)tlv_length;
	request[6] = (uint8_t)(tlv_length >> 8);
	memcpy(request + 7, tlvs, tlv_length);
	qmi_replied = false;
	qmi_reply_length = 0;
	if (qrtr_out_packet(QRTR_DATA, QMI_OUR_PORT, node, port, request, 7u + tlv_length) != 0) {
		printf("adsp845: %u: QMI %s: could not be sent\n", step, what);
		return -1;
	}
	for (unsigned ms = 0; ms < 2000 && !qmi_replied; ms++) {
		sleep_ms(1);
	}
	if (!qmi_replied) {
		printf("adsp845: %u: QMI %s: NO ANSWER in 2 s\n", step, what);
		return -1;
	}
	OSMemoryBarrier();
	{
		uint32_t n = qmi_reply_length;
		char text[3 * 24 + 1] = "";

		for (uint32_t i = 0; i < n && i < 24; i++) {
			snprintf(text + 3 * i, 4, "%02x ", qmi_reply[i]);
		}
		printf("adsp845: %u: QMI %s: the answer (%u bytes): %s\n", step, what, n, text);
		/* the TLVs after the 7 bytes of header: TLV 2 is the result */
		for (uint32_t at = 7; at + 3 <= n;) {
			uint32_t type = qmi_reply[at], length = (uint32_t)(qmi_reply[at + 1] | qmi_reply[at + 2] << 8);

			if (type == 2 && length >= 4 && at + 3 + 4 <= n) {
				uint32_t result = (uint32_t)(qmi_reply[at + 3] | qmi_reply[at + 4] << 8);
				uint32_t error = (uint32_t)(qmi_reply[at + 5] | qmi_reply[at + 6] << 8);

				printf("adsp845: %u: QMI %s: result %u, error %u%s\n", step, what, result, error, result == 0 ? " (DONE)" : " (REFUSED)");
				return (int)result;
			}
			at += 3 + length;
		}
		printf("adsp845: %u: QMI %s: an answer without a result\n", step, what);
		return -1;
	}
}

static bool
mic_slimbus_service(uint32_t *node, uint32_t *port)
{
	for (unsigned ms = 0; ms < 5000; ms += 100) {
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].service == 0x301) {
				*node = qrtr_servers[i].node;
				*port = qrtr_servers[i].port;
				return true;
			}
		}
		sleep_ms(100);
	}
	return false;
}

/* QMI 0x301: select instance 0 as MASTER-run (the DSP is the bus's master), then power ACTIVE (2) or INACTIVE (1) */
static bool
mic_slimbus_power(bool on)
{
	static const uint8_t select[] = { 0x01, 4, 0, 0, 0, 0, 0, 0x10, 4, 0, 2, 0, 0, 0 };
	uint8_t power[] = { 0x01, 4, 0, on ? 2 : 1, 0, 0, 0 };
	uint32_t node = 0, port = 0;

	if (!mic_slimbus_service(&node, &port)) {
		printf("adsp845: %u: the DSP has not told of its SLIMbus service (0x301) in 5 s\n", step);
		return false;
	}
	if (on) {
		printf("adsp845: %u: about to ask the DSP's SLIMbus service (node %u port %u): instance 0, it the master\n", step, node, port);
		if (qmi_ask(node, port, 0x20, select, sizeof(select), "select instance 0, MASTER") != 0) {
			return false;
		}
	}
	printf("adsp845: %u: about to ask the DSP's SLIMbus service: power %s\n", step, on ? "ACTIVE" : "INACTIVE");
	return qmi_ask(node, port, 0x21, power, sizeof(power), on ? "power ACTIVE" : "power INACTIVE") == 0;
}

static struct block mic_ngd = { "slim ngd", 0x171c0000u, 0x2000, 0 };
static struct block mic_bam_top = { "slim bam", 0x17184000u, 0x5000, 0 };        /* (to 0x17189000: BAM_REVISION, the EE masks) */
static struct block mic_bam_pipes = { "slim bam pipes 3, 4", 0x1719a000u, 0x2000, 0 };  /* P_CTRL of pipe n: 0x17184000 + 0x13000 + n * 0x1000 */

/* M2: QMI, and then -- only then -- the NGD's and the BAM's identities read */
static bool
step_mic_qmi(void)
{
	bool ok;

	announce("ASK the DSP's SLIMbus service over QMI (select instance 0 MASTER, power ACTIVE); only if it agrees, READ the NGD's version "
	    "and status and the BAM's revision and pipes; then power INACTIVE. Nothing else is written");
	ok = mic_slimbus_power(true);
	if (!ok) {
		printf("adsp845: %u: the SLIMbus is not powered: the NGD and the BAM are not touched\n", step);
		return false;
	}
	sleep_ms(10);
	printf("adsp845: %u: NGD: version %#x (%u.%u), NGD 1 config %#x, status %#x (logical address %s)\n", step,
	    get(&mic_ngd, "version", 0), get(&mic_ngd, "version", 0) >> 28, get(&mic_ngd, "version", 0) >> 16 & 0xfff,
	    get(&mic_ngd, "ngd 1 config", 0x1000), get(&mic_ngd, "ngd 1 status", 0x1004),
	    (get(&mic_ngd, "ngd 1 status", 0x1004) & 2) ? "SET" : "not set");
	printf("adsp845: %u: BAM: revision %#x, %u pipes, EE 1's interrupt mask %#x; pipe 3 control %#x, pipe 4 control %#x\n", step,
	    get(&mic_bam_top, "revision", 0x1000), get(&mic_bam_top, "pipes", 0x1008) & 0xff, get(&mic_bam_top, "ee 1 mask", 0x4004),
	    get(&mic_bam_pipes, "pipe 3 control", 0), get(&mic_bam_pipes, "pipe 4 control", 0x1000));
	ok = mic_slimbus_power(false);
	printf("adsp845: %u: over: %s\n", step, ok ? "the SLIMbus powered down again" : "the power down NOT confirmed");
	return true;
}

/* steps 18 to 20: none (the numbers are kept for the speaker's later steps) */
static bool
step_none(void)
{
	return true;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link, step_qrtr,
	step_apr, step_tfa, step_amplifier, step_afe, step_stream, step_speaker, step_darwin, step_none, step_none, step_none,
	step_mic_survey, step_mic_qmi,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (adsp8_keep) */

static void
steps_over(uint32_t done)
{
	if (s8_stopping) {
		return;                         /* (step 17 was ended by the run's end, which shuts the DSP down itself) */
	}
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		tell("adsp845: THE AUDIO DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	} else {
		meta_release();                 /* (a sequence that stopped before the start: the header's memory is given back) */
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_adsp_stop(void)
{
	s8_stopping = true;
	if (s8_running) {
		unsigned ms = 0;

		tell("adsp845: the run is about to end: Darwin's sound to the speaker is stopped first (the amplifier off, the stream closed)\n");
		for (; ms < 5000 && s8_running; ms += 10) {
			sleep_ms(10);
		}
		tell("adsp845: %s after %u ms\n", s8_running ? "STEP 17 DID NOT STOP" : "step 17 stopped", ms);
	}
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("adsp845: the audio DSP is off: it had been up%s (node %u, %u services), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, stopped_by_its_word ? "stopped when asked" :
		    "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
hakios_adsp_quiesce(void)
{
	if (s5_amp_on && s5_bus.sda != NULL) {
		/* the amplifier off without a word or a lock: AMPE 0, 10 ms, power down (writes only, no read back) */
		static const uint16_t words[2] = { 0x8220, 0x8201 };

		for (unsigned i = 0; i < 2; i++) {
			bb_start(&s5_bus);
			bb_write(&s5_bus, TFA_ADDRESS << 1);
			bb_write(&s5_bus, 0x00);
			bb_write(&s5_bus, (uint8_t)(words[i] >> 8));
			bb_write(&s5_bus, (uint8_t)words[i]);
			bb_stop(&s5_bus);
			delay(10000);
		}
		s5_amp_on = false;
	}
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	/* (adsp8=8NN: the DSP is left alone: steps 11 and under all need it, the amplifier's probe (12) does not) */
	for (step = flag_alone ? FIRST_INDEPENDENT_STEP : 1; step <= last_step; step++) {
		if (last_step >= MIC_FIRST_STEP && step >= FIRST_INDEPENDENT_STEP && step < MIC_FIRST_STEP) {
			continue;               /* (the microphone's steps: the speaker's, 12 to 20, are not run -- nothing is played) */
		}
		if (!steps[step - 1]()) {
			tell("adsp845: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		tell("adsp845: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	tell("adsp845: over after step %u; %s\n", done, last_write);
}

/* adsp8=N: the OnePlus 6T's audio DSP, steps 1 to N, once the root is there (the image is on it), while the system goes on starting. */
static void
steps_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned waited = 0; !flag_alone && waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	if (rootvnode == NULLVP && !flag_alone) {
		tell("adsp845: no root file system within a minute: the steps are not performed\n");
	} else {
		run_steps();
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (drivers/input/hakios.input.c), beside the modem's. Does nothing without adsp8=N. */
void
hakios_adsp_init(void)
{
	thread_t thread;

	PE_parse_boot_argn("adsp8", &last_step, sizeof(last_step));
	if (last_step == 0) {
		return;
	}
	/* (the boot line is nearly full: what would be three more arguments is the hundreds of this one: adsp8=107 is step 7 with cx.lvl) */
	flag_cx = (last_step / 100) & 1;
	flag_keep = (last_step / 100) & 2;
	flag_again = (last_step / 100) & 4;
	flag_alone = (last_step / 100) & 8;
	last_step %= 100;
	if (last_step == 0) {
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = flag_keep || option("adsp8_keep");
	/* (steps 11 and on are new: each waits after its announcement; from 11 up APR's channel is answered when the DSP opens it, in step 9) */
	new_from = last_step >= 11 ? (flag_alone ? FIRST_INDEPENDENT_STEP : 11) : last_step;
	glink.channel[CHANNEL_APR].wanted = last_step >= 11 && !flag_alone;
	PE_parse_boot_argn("adsp8_pause", &pause_ms, sizeof(pause_ms));
	tell("adsp845: steps %u up to %u%s (cx.lvl %s, kept %s, again %s); the last waits %u ms after each announcement; the steps wait for the "
	    "root file system, and the system goes on starting\n", flag_alone ? FIRST_INDEPENDENT_STEP : 1, last_step,
	    flag_alone ? " (the DSP left alone)" : "", (flag_cx || option("adsp8_cx")) ? "asked for" : "not asked",
	    keep ? "yes" : "no", (flag_again || option("adsp8_again")) ? "yes" : "no", pause_ms);
	if (kernel_thread_start(steps_thread, NULL, &thread) != KERN_SUCCESS) {
		tell("adsp845: no thread: the steps are not performed\n");
		return;
	}
	thread_deallocate(thread);
}
