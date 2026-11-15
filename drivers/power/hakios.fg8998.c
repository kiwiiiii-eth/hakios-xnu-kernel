/*
 * The OnePlus 6T's battery: the PMI8998's fuel gauge and charger, read through the PMIC arbiter that the buttons'
 * driver maps (hakios_pmic_reader, drivers/core/hakios.platform.c), and handed to drivers/power/hakios.battery.c
 * (hakios_battery_report): the sysctls kern.hakios.battery_* and HakiosBattery, the power source Darwin shows.
 *
 * READ ONLY, unless fg_charge=1 (below: two registers of the charger). Otherwise nothing here writes to the PMIC: the
 * charger runs as the boot loader left it (its own limits, its own JEITA protection), and the fuel gauge keeps learning
 * on its own. What is read, every ten seconds (slave 2, the
 * PMI8998; the layout is the phone's own kernel's, LineageOS 22.2 drivers/power/supply/qcom/fg-reg.h, qpnp-fg-gen3.c,
 * and Linux's qcom_smbx.c for the charger):
 *
 *   0x4009/0x400a  the monotonic state of charge and its shadow copy (0..255; read until the two agree)
 *   0x41a0..a1     the battery's voltage, 122.070 uV a unit
 *   0x41a2..a3     its current, 488.281 uA a unit, signed (bit 15): more than 0 while the battery gives
 *   0x4150..51     its temperature, 11 bits, quarters of a kelvin
 *   0x1310 bit 4   USB in (the charger's USBIN interrupt status, as the touch screen's driver reads it)
 *   0x1006 [2:0]   what the charger is doing: 0 trickle, 1 pre-charge, 2 fast, 3 full on, 4 taper, 5 done,
 *                  6 inhibited, 7 disabled
 * and once, at the start, the subtypes of the fuel gauge's two blocks (0x4005: 0x10, 0x4105: 0x11), which say that
 * this is the PMI8998's gauge before anything of it is believed.
 *
 * Boot-arg fg8998=0 leaves it all alone.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern void loader_log_putc(char c);

/* every line to the loader's log as well (where a run's live log reads it; printf alone reaches only the final one) */
static void fg_say(const char *format, ...) __printflike(1, 2);

static void
fg_say(const char *format, ...)
{
	char line[1100];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}
#define printf fg_say

#define FG_SOC                  0x240           /* slave 2, peripheral 0x40: the gauge's state-of-charge block */
#define FG_INFO                 0x241           /* ... 0x41: its battery measurements */
#define CHG_CHARGER             0x210           /* ... 0x10: the charger */
#define CHG_USBIN               0x213           /* ... 0x13: the charger's USB input */
#define REG_SUBTYPE             0x05
#define FG_SOC_SUBTYPE          0x10            /* FG_BATT_SOC_PMI8998 */
#define FG_INFO_SUBTYPE         0x11            /* FG_BATT_INFO_PMI8998 */
#define FG_MONOTONIC_SOC        0x09
#define FG_BATT_TEMP            0x50
#define FG_VBATT                0xa0
#define FG_IBATT                0xa2
#define CHG_STATUS_1            0x06
#define USBIN_RT_STATUS         0x10
#define USBIN_PLUGGED_IN        0x10

#define POLL_SECONDS            10

/*
 * What the charger is set to, read only (boot-arg fg_charger_look=0 leaves it out), once at the start, every minute for
 * ten minutes, then every ten, and whenever USB comes or goes: why the battery still gives while USB is in (11-11: 47% to 37% on the Mac;
 * 11-13: "full on" and yet -8 to -158 mA). Slave 2, the charger's registers as Linux's qcom_smbx.c names them; the input
 * current limits are in 25 mA units.
 */
static const struct {
	uint16_t reg;                   /* slave 2, peripheral and offset: 0x1006 is peripheral 0x10, offset 6 */
	const char *name;
} fg_charger_regs[] = {
	{ 0x1006, "BATTERY_CHARGER_STATUS_1 ([2:0] state)" },
	{ 0x1007, "BATTERY_CHARGER_STATUS_2 (bit 7 input current limited)" },
	{ 0x100a, "BATTERY_CHARGER_STATUS_4 (charge current after JEITA, 25 mA)" },
	{ 0x100d, "BATTERY_CHARGER_STATUS_7" },
	{ 0x1042, "CHARGING_ENABLE_CMD" },
	{ 0x1051, "CHGR_CFG2" },
	{ 0x1060, "PRE_CHARGE_CURRENT_CFG" },
	{ 0x1061, "FAST_CHARGE_CURRENT_CFG (25 mA)" },
	{ 0x1070, "FLOAT_VOLTAGE_CFG" },
	{ 0x1307, "APSD_STATUS" },
	{ 0x1308, "APSD_RESULT_STATUS (bit 0 SDP, 2 CDP, 3 DCP)" },
	{ 0x130b, "TYPE_C_STATUS_1 (bit 7 Rp default, 6 1.5 A, 5 3 A)" },
	{ 0x130c, "TYPE_C_STATUS_2" },
	{ 0x1310, "USBIN_INT_RT_STS" },
	{ 0x1362, "USBIN_OPTIONS_1_CFG" },
	{ 0x1365, "USBIN_LOAD_CFG" },
	{ 0x1366, "USBIN_ICL_OPTIONS (bit 1 USB51 mode, 0 high current mode)" },
	{ 0x1370, "USBIN_CURRENT_LIMIT_CFG (25 mA)" },
	{ 0x1380, "USBIN_AICL_OPTIONS_CFG" },
	{ 0x1607, "ICL_STATUS (the limit in force, 25 mA)" },
	{ 0x1608, "ICL_STATUS + 1" },
	{ 0x160b, "POWER_PATH_STATUS" },
	{ 0x1651, "WD_CFG" },
	{ 0x1653, "SNARL_BARK_BITE_WD_CFG" },
};

static bool fg_read(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count);

/*
 * Charging under this kernel (boot-arg fg_charge=1; the user's decision, 11-13): two registers of the charger written,
 * and nothing else. Run 46 read what the boot loader leaves: USB's input limited to 500 mA (USBIN_CURRENT_LIMIT_CFG
 * 0x1370 = 0x14, in force at ICL_STATUS 0x1607; "input current limited" set) although the Mac's port is a CDP and says
 * 1.5 A on Type-C (0x1308 = 0x04, 0x130b = 0x40), and the battery's fast-charge current at 0 (0x1061). So the phone took
 * 500 mA and the battery gave the rest. Written here: the input limit to fg_input_ma (1000; at most 1500) and the
 * fast-charge current to fg_charge_ma (1000, Linux's qcom_smbx.c's cap "to minimise potential fire hazard risks"; at
 * most 1000), 25 mA a unit, each read back. The charger's own protection (JEITA, its timers) is left as it is; and
 * while the battery is at 45 C or more both go back to what they were and stay so for the rest of the run.
 *
 * Writes through the PMIC arbiter (version 5, the reference phone's addresses: drivers/camera/hakios.torch.c) on the
 * channel of the charger's peripheral, if that channel is ours (owner 0); one at a time with the buttons' driver.
 */
#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

struct fg_writable {
	uint16_t id;                    /* slave << 8 | peripheral */
	uint8_t reg;
	uint8_t was;                    /* as the boot loader left it */
	vm_offset_t channel;            /* its write channel, mapped */
};
static struct fg_writable fg_icl = { .id = 0x213, .reg = 0x70 };      /* USBIN_CURRENT_LIMIT_CFG */
static struct fg_writable fg_fcc = { .id = 0x210, .reg = 0x61 };      /* FAST_CHARGE_CURRENT_CFG */
static bool fg_written, fg_given_back;

static const char *
fg_find_channel(struct fg_writable *w)
{
	static vm_offset_t core, cnfg;
	uint32_t version, channels;

	if (core == 0) {
		core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	}
	version = *(volatile uint32_t *)(core + 0);
	channels = *(volatile uint32_t *)(core + 4) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		return "the arbiter is not of version 5";
	}
	channels = channels == 0 || channels > ARB_MAX_CHANNELS ? ARB_MAX_CHANNELS : channels;
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = *(volatile uint32_t *)(core + ARB_CHANNEL(n));

		if (map != 0 && ((map >> 8) & 0xfff) == w->id && (*(volatile uint32_t *)(cnfg + ARB_OWNER(n)) & 7) == 0) {
			w->channel = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * n, 0x1000);
			return NULL;
		}
	}
	return "no channel of the arbiter for it is ours";
}

/* One register written and read back: NULL, or what went wrong. */
static const char *
fg_put(struct fg_writable *w, uint8_t value)
{
	uint8_t back = 0xff;
	uint32_t status = 0;

	if (w->channel == 0) {
		return "no channel";
	}
	for (int tries = 0; tries < 3; tries++) {
		uint64_t start = mach_absolute_time(), interval;

		nanoseconds_to_absolutetime(2000 * (uint64_t)NSEC_PER_USEC, &interval);
		hakios_spmi_lock();
		*(volatile uint32_t *)(w->channel + CHANNEL_WDATA0) = value;
		*(volatile uint32_t *)(w->channel + CHANNEL_COMMAND) = (uint32_t)w->reg << 4;     /* write, one byte */
		do {
			status = *(volatile uint32_t *)(w->channel + CHANNEL_STATUS);
		} while (!(status & 1) && mach_absolute_time() - start < interval);
		hakios_spmi_unlock();
		if ((status & 0xf) != (1 | 8)) {
			break;          /* (done, or failed; dropped ones -- done and 8 -- are asked for again) */
		}
	}
	if (!(status & 1) || (status & 0xe)) {
		return !(status & 1) ? "no answer from the arbiter" : (status & 4) ? "the arbiter refuses" : "the write failed";
	}
	if (!fg_read(w->id, w->reg, &back, 1) || back != value) {
		return "it does not read back as written";
	}
	return NULL;
}

static void
fg_charge_start(void)
{
	uint32_t input_ma = 1000, charge_ma = 1000;
	const char *problem;

	PE_parse_boot_argn("fg_input_ma", &input_ma, sizeof(input_ma));
	PE_parse_boot_argn("fg_charge_ma", &charge_ma, sizeof(charge_ma));
	input_ma = input_ma < 100 ? 100 : input_ma > 1500 ? 1500 : input_ma;
	charge_ma = charge_ma > 1000 ? 1000 : charge_ma;
	if (hakios_battery_temperature() >= 45) {
		printf("fg8998: CHARGING NOT SET UP: the battery is at %d C\n", hakios_battery_temperature());
		return;
	}
	if ((problem = fg_find_channel(&fg_icl)) != NULL || (problem = fg_find_channel(&fg_fcc)) != NULL ||
	    !fg_read(fg_icl.id, fg_icl.reg, &fg_icl.was, 1) || !fg_read(fg_fcc.id, fg_fcc.reg, &fg_fcc.was, 1)) {
		printf("fg8998: CHARGING NOT SET UP: %s\n", problem != NULL ? problem : "a read failed");
		return;
	}
	fg_written = true;
	if ((problem = fg_put(&fg_icl, (uint8_t)(input_ma / 25))) != NULL ||
	    (problem = fg_put(&fg_fcc, (uint8_t)(charge_ma / 25))) != NULL) {
		printf("fg8998: CHARGING: a write: %s; both given back\n", problem);
		(void)fg_put(&fg_fcc, fg_fcc.was);
		(void)fg_put(&fg_icl, fg_icl.was);
		fg_given_back = true;
		return;
	}
	printf("fg8998: CHARGING SET UP: USB's input limit %u mA (was %u), the battery's fast charge %u mA (was %u); back as they "
	    "were at 45 C\n", input_ma, fg_icl.was * 25, charge_ma, fg_fcc.was * 25);
}

/* every poll while charging is set up: too warm, and both go back for the rest of the run */
static void
fg_charge_watch(void)
{
	if (!fg_written || fg_given_back || hakios_battery_temperature() < 45) {
		return;
	}
	fg_given_back = true;
	printf("fg8998: THE BATTERY IS AT %d C: charging given back (input %s, fast charge %s)\n", hakios_battery_temperature(),
	    fg_put(&fg_icl, fg_icl.was) == NULL ? "back" : "NOT BACK", fg_put(&fg_fcc, fg_fcc.was) == NULL ? "back" : "NOT BACK");
}

static void
fg_charger_look(const char *why)
{
	char line[1100];
	size_t n;

	n = (size_t)snprintf(line, sizeof(line), "fg8998: the charger (%s), read only:", why);
	for (unsigned k = 0; k < sizeof(fg_charger_regs) / sizeof(fg_charger_regs[0]) && n < sizeof(line); k++) {
		uint8_t value = 0;
		uint16_t reg = fg_charger_regs[k].reg;
		bool ok = fg_read((uint16_t)(0x200 | reg >> 8), (uint8_t)reg, &value, 1);

		n += (size_t)snprintf(line + n, sizeof(line) - n, ok ? " %#x=%#04x" : " %#x=?", reg, value);
	}
	printf("%s\n", line);
}

static bool
fg_read(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	return hakios_pmic_reader != NULL && hakios_pmic_reader(id, reg, bytes, count) == 0;
}

/* The state of charge in hundredths of a percent, the way the phone's kernel rounds it (fg_get_msoc); -1 if not read. */
static int32_t
fg_soc(void)
{
	uint8_t cap[2];

	for (int tries = 0; tries < 5; tries++) {
		if (!fg_read(FG_SOC, FG_MONOTONIC_SOC, cap, 2)) {
			return -1;
		}
		if (cap[0] == cap[1]) {
			if (cap[0] == 255) {
				return 10000;
			}
			if (cap[0] == 0) {
				return 0;
			}
			return 100 + ((int32_t)cap[0] - 1) * 9800 / 253;        /* 1..254 to 1..99 percent */
		}
	}
	return -1;
}

/*
 * The PMIC's real-time clock (PM8998, slave 0, peripheral 0x60, RTC_RW; RTC_READ0..3 at 0x48: seconds, little-endian, as
 * Linux's rtc-pm8xxx reads them, read again from the low byte until a read does not straddle a tick). It goes on while
 * the phone is off, and it is a count, not the date (Android keeps its own offset to it; it read 60110539 on the
 * reference phone, which as a date is 1971): userland/src/rtcclock.c keeps the offset in /private/var and sets the clock
 * from it at boot, so that a phone that boots with no host to give it the time (Beta 0.4) has it all the same. Read here,
 * in this thread, with the battery's registers every POLL_SECONDS (the shared reader takes one caller at a time), and
 * given as kern.hakios.rtc_now: the count now, from the last read and the time since (mach_continuous_time, which goes
 * on through sleep); -1 when it was never read. Nothing is written to the clock.
 */
#define RTC_RW                  0x060           /* slave 0, peripheral 0x60 */
#define RTC_TYPE                0x07
#define RTC_REG_TYPE            0x04
#define RTC_READ0               0x48

static uint32_t rtc_last;
static uint64_t rtc_last_at;                    /* mach_continuous_time() of the read; 0: never read */
static bool rtc_checked, rtc_ok;

static void
rtc_poll(void)
{
	uint8_t b[4], again = 0, type = 0;

	if (!rtc_checked) {
		rtc_checked = true;
		rtc_ok = fg_read(RTC_RW, RTC_REG_TYPE, &type, 1) && type == RTC_TYPE;
		printf("fg8998: the real-time clock (PM8998 0x6000): %s (type %#x)\n", rtc_ok ? "read for kern.hakios.rtc_now" :
		    "NOT the clock, not read", type);
	}
	for (int tries = 0; rtc_ok && tries < 3; tries++) {
		if (!fg_read(RTC_RW, RTC_READ0, b, 4) || !fg_read(RTC_RW, RTC_READ0, &again, 1)) {
			return;
		}
		if (again == b[0]) {
			rtc_last = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
			rtc_last_at = mach_continuous_time();
			return;
		}
	}
}

static int
fg_sysctl_rtc_now SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int64_t value = -1;

	if (rtc_last_at != 0) {
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_continuous_time() - rtc_last_at, &ns);
		value = (int64_t)rtc_last + (int64_t)(ns / NSEC_PER_SEC);
	}
	return SYSCTL_OUT(req, &value, sizeof(value));
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, rtc_now, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, fg_sysctl_rtc_now, "Q",
    "the PMIC's real-time clock now, in its seconds (a count, not the date: userland/src/rtcclock.c); -1: never read");

static void
fg_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const doing[8] = { "trickle", "pre-charge", "fast", "full on", "taper", "done", "inhibited",
		                              "disabled" };
	uint8_t subtype[2] = { 0, 0 };
	uint32_t charger_look = 1, charge_on = 0;
	int was_plugged = -1;

	for (int i = 0; i < 60 && hakios_pmic_reader == NULL; i++) {
		delay_for_interval(500, NSEC_PER_MSEC);         /* the buttons' driver maps the arbiter */
	}
	if (!fg_read(FG_SOC, REG_SUBTYPE, &subtype[0], 1) || !fg_read(FG_INFO, REG_SUBTYPE, &subtype[1], 1)) {
		printf("fg8998: the PMIC cannot be read (buttons=0?): no battery\n");
		return;
	}
	if (subtype[0] != FG_SOC_SUBTYPE || subtype[1] != FG_INFO_SUBTYPE) {
		printf("fg8998: the blocks at 0x4000 and 0x4100 are subtypes %#x and %#x, not the PMI8998's gauge: no battery\n",
		    subtype[0], subtype[1]);
		return;
	}
	printf("fg8998: the PMI8998's fuel gauge (subtypes %#x, %#x); read every %u s, nothing written\n", subtype[0],
	    subtype[1], POLL_SECONDS);
	PE_parse_boot_argn("fg_charger_look", &charger_look, sizeof(charger_look));
	PE_parse_boot_argn("fg_charge", &charge_on, sizeof(charge_on));
	if (charger_look) {
		for (unsigned k = 0; k < sizeof(fg_charger_regs) / sizeof(fg_charger_regs[0]); k++) {
			printf("fg8998: %#x %s\n", fg_charger_regs[k].reg, fg_charger_regs[k].name);
		}
		fg_charger_look("at the start");
	}

	for (unsigned tick = 0;; tick++) {
		struct hakios_battery_state state = { .size = sizeof(state) };

		rtc_poll();
		uint8_t v[2] = { 0, 0 }, i[2] = { 0, 0 }, t[2] = { 0, 0 }, usb = 0, chg = 0;
		int32_t soc = fg_soc(), ibatt_ua, temp_k4;
		bool plugged, ok;

		ok = soc >= 0 && fg_read(FG_INFO, FG_VBATT, v, 2) && fg_read(FG_INFO, FG_IBATT, i, 2) &&
		    fg_read(FG_INFO, FG_BATT_TEMP, t, 2) && fg_read(CHG_USBIN, USBIN_RT_STATUS, &usb, 1) &&
		    fg_read(CHG_CHARGER, CHG_STATUS_1, &chg, 1);
		if (ok) {
			plugged = (usb & USBIN_PLUGGED_IN) != 0;
			ibatt_ua = (int32_t)((int64_t)(int16_t)(i[1] << 8 | i[0]) * 488281 / 1000);
			temp_k4 = (t[1] & 0x7) << 8 | t[0];
			state.percent_hundredths = soc;
			state.voltage_uv = (int32_t)((uint64_t)(v[1] << 8 | v[0]) * 122070 / 1000);
			state.current_ua = -ibatt_ua;                   /* HakiOS's: more than 0 while the battery takes charge */
			state.temperature_hundredths = (temp_k4 * 25) - 27315;
			state.present = 1;
			state.external = plugged;
			if (!plugged) {
				state.status = 2;                       /* discharging */
			} else if ((chg & 7) <= 4) {
				state.status = 1;                       /* charging */
			} else if ((chg & 7) == 5) {
				state.status = 4;                       /* full */
			} else {
				state.status = 3;                       /* not charging */
			}
			state.usb_type = plugged ? 1 : 0;
			hakios_battery_report(&state);
			/* (every minute for the first ten, where a run's log keeps it; then every ten minutes) */
			if (charger_look && (was_plugged != (int)plugged || (tick != 0 && tick % (tick < 60 ? 6 : 60) == 0))) {
				fg_charger_look(was_plugged != (int)plugged ? (plugged ? "USB in" : "USB out") : "a while on");
			}
			was_plugged = plugged;
			if (tick == 1 && charge_on) {
				fg_charge_start();       /* (after the first reading: the battery's temperature is known) */
				fg_charger_look("charging set up");
			}
			fg_charge_watch();
			if (tick % 6 == 0) {
				printf("fg8998: %d.%02d%%, %d mV, %d mA (gauge %d), %d.%02d C; USB %s, charger %s\n", soc / 100,
				    soc % 100, state.voltage_uv / 1000, state.current_ua / 1000, ibatt_ua / 1000,
				    state.temperature_hundredths / 100, (state.temperature_hundredths < 0 ? -state.temperature_hundredths :
				    state.temperature_hundredths) % 100, plugged ? "in" : "out", doing[chg & 7]);
			}
		} else if (tick % 6 == 0) {
			printf("fg8998: a reading failed (state of charge %d)\n", soc);
		}
		delay_for_interval(POLL_SECONDS * 1000, NSEC_PER_MSEC);
	}
}

void
hakios_fg8998_init(void)
{
	uint32_t enabled = 1;
	thread_t thread;

	PE_parse_boot_argn("fg8998", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (kernel_thread_start(fg_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
