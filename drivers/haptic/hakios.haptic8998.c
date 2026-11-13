/*
 * The vibration motor of the OnePlus 6T (fajita): a linear resonant actuator driven by the PMI8998's haptics module (SPMI
 * slave 3, registers 0xc000; downstream "qcom,qpnp-haptics", LineageOS's leds-qpnp-haptics.c; the mainline device tree
 * has no node for it). Not the reference phone's Awinic chip on I2C (hakios.haptic.c, driver HAPTIC), whose Darwin side
 * it keeps: the audio server's "Actuator" device (userland/src/audio_plugin.c) writes Darwin's haptic waveforms to
 * /dev/hakios-haptic as samples, and the motor follows them.
 *
 * OnePlus's own values (fajita.dtsi &pmi8998_haptics, over pmi8998.dtsi's): an LRA, VMAX 2088 mV, the drive period 4255
 * us (235 Hz), a sine, the brake <3 3 3 3>, 800 mA, short-circuit debounce 8 cycles, auto-resonance zxd-eop every 32. Its
 * play mode is "buffer" (sample patterns); here it is "direct": the module drives at VMAX for as long as PLAY is set,
 * which a tap and a vibration of Darwin's both are (as long as Darwin's waveform goes on, its loudness as VMAX). The
 * PMI8998 keeps auto-resonance's enable behind its secure access (TEST2, 0xe3): it is not touched; the drive period is
 * OnePlus's. Registers (leds-qpnp-haptics.c):
 *
 *   0x0a  STATUS_1: [1] busy, [3] short circuit      0x51  VMAX [5:1], 116 mV a step
 *   0x10  INT_RT_STS: [0] short circuit              0x52  ILIM [0]: 800 mA
 *   0x46  EN_CTL [7]: the module                     0x53  the short circuit's debounce [2:0]
 *   0x48  EN_CTL2 [0]: the brake                     0x54/0x55  the drive period [11:0], 5 us a step
 *   0x4c  CFG1 [0]: 0 an LRA                         0x59  SC_CLR [0]
 *   0x4d  CFG2 [0]: 0 a sine                         0x5c  the brake pattern (two bits a step)
 *   0x4e  SEL [5:4]: the waveform's source, 0 direct 0x70  PLAY [7]
 *   0x4f  LRA_AUTO_RES: mode [6:4], high-Z [3:2], calibration period [1:0]
 *
 * Writing to a PMIC is not like writing anywhere else, as in hakios.torch8998.c (whose arbiter code this is): only the
 * module's registers above, each read back, any surprise and nothing more is written for the run.
 *
 * Nothing here is done unless a boot-arg asks for it (the reference phone's numbers):
 *
 *   haptic=1   a look, reading only: the arbiter's channel and who may write through it, the module's type, its registers
 *   haptic=2   ... and OnePlus's configuration written (every write read back); nothing moves
 *   haptic=4   ... and one buzz of haptic_ms (100; at most 500)
 *   haptic=5   ... configured, and the motor for Darwin until the run ends (/dev/hakios-haptic)
 *
 * Limits, whatever asks: OnePlus's VMAX at most (a softer waveform of Darwin's, down to five eighths of it); no buzz
 * longer than HAPTIC_MAX_MS, after which the motor rests for as long; a short circuit the module reports, a write that
 * does not read back, and it is stopped and nothing more is done for the run. At the run's end (and on a panic's way
 * out) it is stopped and the module disabled.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

/*
 * The events that matter (lit, off, cut, trouble) are also kept, and said again at the run's end:
 * the log of a long run keeps its beginning and its end, and these happen in between.
 */
#define NOTES                   24
static char notes[NOTES][200];
static unsigned noted;

static void note(const char *format, ...) __printflike(1, 2);

static void
note(const char *format, ...)
{
	char *slot = notes[noted++ % NOTES];
	uint64_t ns;
	va_list list;
	int n;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	n = snprintf(slot, sizeof(notes[0]), "%llu.%03llu s: ", ns / NSEC_PER_SEC, ns / NSEC_PER_MSEC % 1000);
	va_start(list, format);
	vsnprintf(slot + n, sizeof(notes[0]) - (size_t)n, format, list);
	va_end(list);
	say("haptic: %s\n", slot);
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- the PMIC arbiter (version 5) ---------------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL    /* the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* which system may write through each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL    /* writes: 0x10000 per channel */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long"; [11:4] register, [2:0] bytes - 1 */
#define COMMAND_READ            (1u << 27)      /* "extended register read long" */
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral */
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;          /* its write channel, mapped (only the haptics module's) */
};
static struct peripheral motor = { .name = "the PMI8998's haptics module", .id = 0x3c0 };

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/* (the arbiter one transfer at a time with the buttons' driver; at the run's end the others are stopped, perhaps holding it) */
static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

/*
 * `count' (1 to 8) registers from `reg' on; NULL, or what went wrong. A transfer the arbiter drops
 * now and then (the buttons' driver sees one in a thousand or so, from whatever else is on the bus)
 * is asked for again, twice at most.
 */
static const char *spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);

static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	const char *problem;
	uint32_t data[2];

	if (!p->found) {
		return "no channel";
	}
	lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	unlock();
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* ---- the haptics module ---------------------------------------------------------------------------- */

#define HAP_TYPE                0x04
#define HAP_SUBTYPE             0x05
#define HAP_STATUS_1            0x0a
#define  STATUS1_SC             0x08
#define HAP_INT_RT_STS          0x10
#define  RT_STS_SC              0x01
#define HAP_EN_CTL              0x46
#define HAP_EN_CTL2             0x48
#define HAP_CFG1                0x4c
#define HAP_CFG2                0x4d
#define HAP_SEL                 0x4e
#define HAP_LRA_AUTO_RES        0x4f
#define HAP_VMAX_CFG            0x51
#define HAP_ILIM_CFG            0x52
#define HAP_SC_DEB              0x53
#define HAP_RATE_CFG1           0x54
#define HAP_RATE_CFG2           0x55
#define HAP_SC_CLR              0x59
#define HAP_BRAKE               0x5c
#define HAP_PLAY                0x70

#define VMAX_MV                 2088                            /* OnePlus's */
#define VMAX_CODE               ((VMAX_MV + 58) / 116)          /* 18: 2088 mV */
#define RATE_CODE               (4255 / 5)                      /* OnePlus's drive period: 851 */
#define HAPTIC_MAX_MS           1000

static const struct {
	uint8_t reg, mask, value;
} configuration[] = {
	{ HAP_CFG1, 0x01, 0x00 },               /* an LRA */
	{ HAP_LRA_AUTO_RES, 0x7f, 0x47 },       /* zxd-eop (4), high-Z opt1 (1), calibration every 32 periods (3) */
	{ HAP_SEL, 0x30, 0x00 },                /* direct */
	{ HAP_VMAX_CFG, 0x3e, VMAX_CODE << 1 },
	{ HAP_ILIM_CFG, 0x01, 0x01 },           /* 800 mA */
	{ HAP_SC_DEB, 0x07, 0x01 },             /* 8 cycles */
	{ HAP_CFG2, 0x01, 0x00 },               /* a sine */
	{ HAP_RATE_CFG1, 0xff, RATE_CODE & 0xff },
	{ HAP_RATE_CFG2, 0x0f, RATE_CODE >> 8 },
	{ HAP_EN_CTL2, 0x01, 0x01 },            /* the brake on */
	{ HAP_BRAKE, 0xff, 0xff },              /* <3 3 3 3>: VMAX each step */
};

static const uint8_t writable_regs[] = {
	HAP_EN_CTL, HAP_EN_CTL2, HAP_CFG1, HAP_CFG2, HAP_SEL, HAP_LRA_AUTO_RES, HAP_VMAX_CFG, HAP_ILIM_CFG, HAP_SC_DEB,
	HAP_RATE_CFG1, HAP_RATE_CFG2, HAP_SC_CLR, HAP_BRAKE, HAP_PLAY,
};

/* set only while stop() runs: its writes may pass the `broken' latch (a running motor must always get its stop) */
static volatile bool forcing;
static bool configured, playing;

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		note("%s: no more writing to the haptics module for the rest of the run", why);
	}
}

static bool
get(uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(&motor, reg, value, 1);

	if (problem != NULL) {
		say("haptic: reading %#04x: %s\n", reg, problem);
		return false;
	}
	return true;
}

/* One register of the module (of the list only) written, and read back in the bits of `check'. */
static bool
put_checked(uint8_t reg, uint8_t value, uint8_t check)
{
	const char *problem = NULL;
	uint8_t back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if ((broken && !forcing) || !allowed || !motor.writable || motor.write == 0 || motor.id != 0x3c0) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(motor.channel)) >> 8) & 0xfff) != motor.id) {
		give_up("the arbiter's channel is no longer the haptics module's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(motor.write + CHANNEL_WDATA0, value);
		wr(motor.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(motor.write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = spmi_read(&motor, reg, &back, 1)) != NULL) {
		say("haptic: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if ((back & check) != (value & check)) {
		say("haptic: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	return (now & mask) == (value & mask) || put_checked(reg, (uint8_t)((now & ~mask) | (value & mask)), 0xff);
}

static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("haptic: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	if (!p->found) {
		say("haptic: no channel of the arbiter is %s\n", p->name);
	}
}

static void
show_registers(void)
{
	uint8_t r[8];
	char line[200];

	for (unsigned base = 0; base < 0x100; base += 0x20) {
		int n = snprintf(line, sizeof(line), "haptic: %#04x:", base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			if (spmi_read(&motor, (uint8_t)(base + i), r, 8) != NULL) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " (unreadable)");
				continue;
			}
			for (unsigned j = 0; j < 8; j++) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
			}
		}
		say("%s\n", line);
	}
}

/* NULL, or why the motor is to stop now */
static const char *
trouble(void)
{
	static char why[120];
	uint8_t rt = 0, status = 0;

	if (!get(HAP_INT_RT_STS, &rt) || !get(HAP_STATUS_1, &status)) {
		return "a read of the flags that failed";
	}
	if ((rt & RT_STS_SC) || (status & STATUS1_SC)) {
		snprintf(why, sizeof(why), "the module says short circuit (interrupt status %#x, status %#x)", rt, status);
		return why;
	}
	return NULL;
}

static bool
configure(void)
{
	for (size_t i = 0; !configured && i < sizeof(configuration) / sizeof(configuration[0]); i++) {
		if (!put_bits(configuration[i].reg, configuration[i].mask, configuration[i].value)) {
			return false;
		}
	}
	if (!configured) {
		configured = true;
		say("haptic: configured as OnePlus's: an LRA, VMAX %u mV, the drive period %u us, direct, the brake on\n", VMAX_MV,
		    RATE_CODE * 5);
	}
	return true;
}

/* the motor driven at `vmax' (in 116 mV) until stop() */
static bool
start(uint8_t vmax)
{
	const char *why;

	if (vmax == 0 || vmax > VMAX_CODE) {
		vmax = VMAX_CODE;
	}
	if (!playing && (why = trouble()) != NULL) {
		note("not started: %s", why);
		give_up("trouble before the motor");
		return false;
	}
	if (!configure() || !put_bits(HAP_VMAX_CFG, 0x3e, (uint8_t)(vmax << 1))) {
		return false;
	}
	if (!playing) {
		playing = true;         /* (before the writes: one that lands but fails its read-back still gets a stop()) */
		if (!put_checked(HAP_EN_CTL, 0x80, 0x80) || !put_checked(HAP_PLAY, 0x80, 0x00)) {
			return false;
		}
	}
	return true;
}

static bool
stop(void)
{
	bool ok;

	forcing = true;
	ok = put_checked(HAP_PLAY, 0x00, 0x00);
	ok = put_checked(HAP_EN_CTL, 0x00, 0x80) && ok;
	forcing = false;
	playing = !ok && playing;
	return ok;
}

/* ---- haptic=5: the motor for Darwin (hakios.haptic.c's service, with a buzz for each of Darwin's waveforms) ---- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define SETTLE_US               8000            /* how long Darwin's waveform is listened to before a buzz: its loudness */
#define TAP_US                  25000           /* the least a buzz lasts */
#define FOLLOW_US               20000           /* how often a vibration's strength follows Darwin's */
#define TOLD_EVENTS             24

static volatile uint64_t heard_us, quiet_us, onset_us;
static volatile uint32_t heard_peak;
static uint64_t written_frames;
static unsigned taps, loops, capped;

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

static void
heard(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
		onset_us = now;
		heard_peak = 0;
	}
	heard_us = now;
	if (peak > heard_peak) {
		heard_peak = peak;
	}
}

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		heard(peak);
	}
	return error;
}

/* (hakios.mic.c's test stand-in for Darwin's waveform, as on the reference phone) */
void
hakios_haptic_pretend(uint32_t peak)
{
	heard(peak);
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* VMAX for Darwin's loudness: OnePlus's from a third of full scale up, down to five eighths of it for the softest */
static uint8_t
vmax_for(uint32_t peak)
{
	uint32_t top = VMAX_CODE, low = top * 5 / 8, third = 32767 / 3;

	return (uint8_t)(peak >= third ? top : low + (top - low) * peak / third);
}

static void
serve(void)
{
	enum { IDLE, PLAYING, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, buzz_onset = 0, followed = 0;
	uint8_t playing_vmax = 0;
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT DARWIN PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends\n");
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;

		switch (state) {
		case IDLE:
			if (active && onset_us != buzz_onset && now - onset_us >= SETTLE_US) {
				playing_vmax = vmax_for(peak);
				if (!start(playing_vmax)) {
					break;
				}
				state = PLAYING;
				since = followed = now;
				buzz_onset = onset_us;
				taps++;
				if (taps <= TOLD_EVENTS) {
					say("haptic: buzz %u: Darwin's waveform peaks at %u of 32768: VMAX %u mV\n", taps, peak,
					    playing_vmax * 116);
				}
			}
			break;
		case PLAYING: {
			bool goes_on = active && quiet_us < buzz_onset;         /* no silence since it began */
			bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

			if (goes_on && !cut && now - followed >= FOLLOW_US) {
				uint8_t vmax = vmax_for(peak);

				followed = now;
				heard_peak = 0;
				if (peak != 0 && vmax != playing_vmax) {
					if (!put_bits(HAP_VMAX_CFG, 0x3e, (uint8_t)(vmax << 1))) {
						break;
					}
					playing_vmax = vmax;
				}
			}
			if (cut || (!goes_on && now - since >= TAP_US)) {
				if (now - since > 2 * TAP_US) {
					loops++;
				}
				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				if (stop()) {
					state = cut ? REST : IDLE;
					if (cut || (now - since > 2 * TAP_US && loops <= TOLD_EVENTS)) {
						say("haptic: a vibration %s after %llu ms\n", cut ? "CUT at the limit" : "ended with Darwin's",
						    (now - since) / 1000);
					}
				}
			}
			break;
		}
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (state == PLAYING && now - last_look > 50000) {
			const char *why;

			last_look = now;
			if ((why = trouble()) != NULL) {
				note("STOPPED: %s", why);
				(void)stop();
				give_up("trouble while playing");
				break;
			}
		}
		wait_ms(4);
	}
	if (playing) {
		(void)stop();
	}
	say("haptic: the motor is given up on (%u buzzes, %u of them vibrations, %u cut at %u ms)\n", taps, loops, capped,
	    HAPTIC_MAX_MS);
}

/* ---- the start --------------------------------------------------------------------------------------- */

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version, channels;
	uint8_t id[2] = { 0, 0 };

	say("haptic: level %u: %s\n", level, level == 1 ? "a look, reading only" : level < 4 ? "configured, nothing moves" :
	    level == 4 ? "one buzz" : "the motor for Darwin");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("haptic: PMIC arbiter version %#x, not 5: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_channel(&motor, channels);
	if (!motor.found || spmi_read(&motor, HAP_TYPE, id, 2) != NULL) {
		say("haptic: the haptics module cannot be read\n");
		goto out;
	}
	say("haptic: the module says type %#x, subtype %#x\n", id[0], id[1]);
	show_registers();
	if (level < 2) {
		goto out;
	}
	if (!motor.writable) {
		say("haptic: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	motor.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * motor.channel, 0x1000);
	if (!configure()) {
		goto out;
	}
	show_registers();
	if (level == 4) {
		uint32_t ms = 100;
		const char *why = NULL;

		PE_parse_boot_argn("haptic_ms", &ms, sizeof(ms));
		ms = ms < 10 ? 10 : ms > 500 ? 500 : ms;
		if (start(VMAX_CODE)) {
			uint8_t status = 0;

			wait_ms(10);
			(void)get(HAP_STATUS_1, &status);
			note("buzzing at %u mV for %u ms; 10 ms on, the status is %#x (%s)", VMAX_CODE * 116, ms, status,
			    (status & 0x02) ? "busy: it plays" : "NOT busy");
			for (uint32_t t = 10; t < ms && (why = trouble()) == NULL; t += 10) {
				wait_ms(10);
			}
			if (why != NULL) {
				note("STOPPED early: %s", why);
			}
			note("%s", stop() ? "stopped" : "COULD NOT BE STOPPED");
		} else if (playing) {
			(void)stop();
		}
	} else if (level == 5) {
		serve();
	}
out:
	say("haptic: done\n");
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 5 ? 5 : level;
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the motor stopped and the module off, if it was ever configured. */
void
hakios_haptic_quiesce(void)
{
	if (level < 2 || motor.write == 0) {
		return;
	}
	leaving = true;
	if (playing) {
		broken = false;
		say("haptic: at the run's end: the motor %s\n", stop() ? "stopped" : "COULD NOT BE STOPPED");
	}
	if (level == 5) {
		say("haptic: at the run's end: %u buzzes (%u vibrations, %u cut), from %llu frames Darwin wrote\n", taps, loops,
		    capped, written_frames);
	}
	say("haptic: what happened in this run (%u events):\n", noted);
	for (unsigned i = noted > NOTES ? noted - NOTES : 0; i < noted; i++) {
		say("haptic:   %s\n", notes[i % NOTES]);
	}
}
