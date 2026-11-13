/*
 * The OnePlus 6T's battery: the PMI8998's fuel gauge and charger, read through the PMIC arbiter that the buttons'
 * driver maps (hakios_pmic_reader, drivers/core/hakios.platform.c), and handed to drivers/power/hakios.battery.c
 * (hakios_battery_report): the sysctls kern.hakios.battery_* and HakiosBattery, the power source Darwin shows.
 *
 * READ ONLY. Nothing here writes to the PMIC: the charger runs as the boot loader left it (its own limits, its own
 * JEITA protection), and the fuel gauge keeps learning on its own. What is read, every ten seconds (slave 2, the
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
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

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

static void
fg_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const doing[8] = { "trickle", "pre-charge", "fast", "full on", "taper", "done", "inhibited",
		                              "disabled" };
	uint8_t subtype[2] = { 0, 0 };
	uint32_t charger_look = 1;
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
	if (charger_look) {
		for (unsigned k = 0; k < sizeof(fg_charger_regs) / sizeof(fg_charger_regs[0]); k++) {
			printf("fg8998: %#x %s\n", fg_charger_regs[k].reg, fg_charger_regs[k].name);
		}
		fg_charger_look("at the start");
	}

	for (unsigned tick = 0;; tick++) {
		struct hakios_battery_state state = { .size = sizeof(state) };
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
