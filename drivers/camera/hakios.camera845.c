/*
 * The cameras of the OnePlus 6T (fajita, SDM845): hakios.camera.c's copy for the 6T's sensors, supplies and the
 * Titan 170 camera subsystem. The sensors (Android's cam-sensor0..2 in the phone's own dtbo, overlay of project
 * 0x4971; the register tables from the sensors' module files, tools/camsensor.py "ParameterParser V1.1.0"):
 *
 *   index 1, rear 0, the main camera: a Sony IMX519 (16 MP, LiteOn module), CCI master 0 (GPIO 17 data, 18 clock),
 *            slave 0x1a, chip ID 0x0519 at 0x0016; MCLK0 (GPIO 13), reset GPIO 28, VANA switched by GPIO 27, VDIG by
 *            GPIO 8; three C-PHY trios into CSIPHY 0
 *   index 0, rear 1, the second rear camera: a Sony IMX376K (20 MP), CCI master 1 (GPIO 19/20), slave 0x10, chip ID
 *            0x0376; MCLK2 (GPIO 15), reset GPIO 23, VANA GPIO 78, VDIG GPIO 79; four D-PHY lanes into CSIPHY 1
 *   index 2, the front camera: a Sony IMX371 (16 MP), CCI master 0 (GPIO 17/18), slave 0x10, chip ID 0x0371; MCLK1
 *            (GPIO 14), reset GPIO 9, VANA GPIO 104, VDIG GPIO 117; four D-PHY lanes into CSIPHY 2
 *
 * (The indices are the frames' header's: 1 is "the wide one", the back camera of Darwin's capture stack, 2 the front
 * one; 0 is the slot of upstream's macro camera.)
 *
 *   supplies   VANA and VDIG: a regulator fed from the PMI8998's BOB, switched by a TLMM GPIO (output high, no
 *              pull, 2 mA); VIO: the PM8998's LVS1 (1.8 V), a switch that RPMh holds ("vsa1"), voted on through
 *              DRV 2's second TCS and given back at the end. BOB itself is looked at (SPMI, reading only) and
 *              voted on ("bobb1") only if nobody had it on.
 *   MCLK       24 MHz, the module files' rate: camcc's PLL2 (Fabia, 960 MHz, its even output at 480) / 10, M/N 1/2,
 *              as camcc-sdm845.c's ftbl_cam_cc_mclk0_clk_src; under the titan top power domain (camcc 0xb134)
 *   power-up   the module files' (all three): VANA, VDIG, VIO, MCLK, reset high, 1 ms after each; power-down the
 *              reverse. A step of a supply is "on" in the power-up and "off" in the power-down (the V1.1.0 tables
 *              give 0 for every supply in both: Android's sensor driver switches regulators by the sequence)
 *   I2C        by hand on the CCI master's pins, as hakios.camera.c does it
 *
 * Nothing here is done unless a boot-arg asks for it, as in hakios.camera.c:
 *
 *   cam=1   a look, reading only: the camera's clocks and power domain, the pins, LVS1 and BOB (SPMI reads), RPMh's
 *           votes for them in DRV 2, the SMMU, camcc's PLLs
 *   cam=2   ... and the sensor powered up as Android powers it, its chip ID read, and powered down again
 *   cam=3   the CSID's test pattern through CSID-lite's RDI 0 and VFE-lite into memory (xl camera)
 *   cam=4   the sensor's own frames that way, through its CSIPHY
 *   cam=5   the frames for Darwin (/dev/hakios-camera), as hakios.camera.c's
 *
 * Boot-arg cam_sensor=main|second|front picks one (default: main).
 *
 * The camera subsystem (Titan 170, CPAS 0x170110), from the base device tree, camcc-sdm845.c and Android's own
 * camera driver (msm-4.9, OnePlus's sdm845 tree: cam_ife_csid_lite170.h, cam_vfe_lite170.h, cam_csiphy_1_0_hwreg.h),
 * read as data: CSID-lite at 0x0acc8000 and VFE-lite at 0x0acc4000 (the same places and register layout as the
 * SM7325's lite blocks that hakios.camera.c drives), CSIPHY n at 0x0ac65000 + 0x1000 n (v1.0: D-PHY and C-PHY), the
 * IFEs' SMMU streams 0x808, 0x810-0x818, 0xc08, 0xc10-0xc18 (matched as one: 0x800, mask 0x41f).
 *
 * Limits, whatever asks: only the registers named here are written, each read back where it keeps what it is given;
 * any fault and everything is switched off in the reverse order and nothing more is done in the run; at the run's
 * end (and on a panic's way out) whatever was switched on is switched off; whatever was found on is left on.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.camera.h>
#include <sys/hakios.input.h>

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
	for (const char *c = line; *c != '\0'; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
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

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("camera: %s: nothing more is written in this run\n", why);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

/*
 * SDM845's TLMM is three tiles (pinctrl-sdm845.c: NORTH 0x3900000, SOUTH 0x3d00000, EAST 0x3500000), each pin's
 * registers at its tile + 0x1000 x the pin's number: [1:0] pull, [5:2] function, [8:6] drive, [9] output; then the
 * in/out register (bit 0 in, bit 1 out). Which tile each of the 150 pins is in, from pinctrl-sdm845.c's groups:
 */
#define TLMM_EAST_PHYS          0x03500000UL
#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_SOUTH_PHYS         0x03d00000UL
#define TLMM_TILE_SIZE          0x100000UL
#define TLMM_PINS               150
static const char tlmm_tiles[TLMM_PINS + 1] =
    "EEEENNNNEEEESSSSSSSSSSSSSSSEEEENNNNSSSNESEEEEEEEENNNNNNNNNNNNNNNNNNNNEEEEEEEEEENNNNNNEEEESSSSSSSSNNNNNNNNNNNNNNNNNN"
    "NNNNNNNEEEEENNNNNNNNNNNNNNNNNNNNNNN";
static vm_offset_t tlmm, tlmm_north, tlmm_south, tlmm_east;   /* (tlmm: non-zero once they are mapped) */

static vm_offset_t
gpio_regs(unsigned n)
{
	char tile = n < TLMM_PINS ? tlmm_tiles[n] : '?';

	return (tile == 'N' ? tlmm_north : tile == 'S' ? tlmm_south : tlmm_east) + 0x1000UL * n;
}

#define GPIO_CFG(n)             gpio_regs(n)            /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (gpio_regs(n) + 4)      /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)
#define CFG_FUNCTION(f)         ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)

static void
tlmm_map(void)
{
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	tlmm_east = ml_io_map(TLMM_EAST_PHYS, TLMM_TILE_SIZE);
	tlmm = tlmm_north;
}

/*
 * The three sensors, as the phone's dtbo has them (cam-sensor0 the main one, cam-sensor1 the second rear one,
 * cam-sensor2 the front one): the pins of their CCI master (taken by hand), the reset, MCLK (camcc RCG and branch, the
 * pin and its function 1, cam_mclk, at 2 mA as Android's cam_sensor_*_mclk_active), the supplies the module file's
 * power sequence switches, the CSIPHY with its clocks, how the sensor's exposure registers are laid out. In the frames'
 * header's order (index 1 is the back camera Darwin's capture stack takes).
 */
enum { SUPPLY_NONE, SUPPLY_GPIO, SUPPLY_RPMH };
struct supply {
	uint8_t         kind;
	uint16_t        mv;             /* (what it gives, for the log) */
	uint16_t        gpio, ramp_us;  /* SUPPLY_GPIO: the pin, active high, and a wait after it */
	const char *    rpmh;           /* SUPPLY_RPMH: RPMh's name of the switch ("vsa1": the PM8998's LVS1) */
};
struct sensor_hw {
	const char *    name;
	const char *    tab;            /* in fw/ of the small image (tools/camsensor.py) */
	const char *    bus_name;
	unsigned        sda, scl, reset;
	uint32_t        mclk_cmd, mclk_cbcr;
	unsigned        mclk_pin;
	struct supply   vana, vdig, vio;
	unsigned        csiphy;
	uint32_t        phy_cbcr, timer_cmd, timer_cbcr;
	uint8_t         regs;           /* the exposure registers: REGS_SONY */
	uint8_t         bayer;          /* the picture's order as it comes here (0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR) */
	uint8_t         pattern_bayer;  /* ... and its own colour bars' (none of the 6T's module files has them) */
	const char *    chip;           /* for the frames' header */
	uint16_t        gain_unity, gain_max;   /* the gain as the frames' header and the program count it: 1x, and the most */
	struct supply   vaf;            /* the lens motor's supply (SUPPLY_NONE: a fixed-focus lens) ... */
	uint8_t         af_address, af_sa_address;  /* ... and the motor on the sensor's CCI master (7-bit): the modules come with an
	                                             * LC898217XC or an SA3103 (two module files each) */
	bool            quad;           /* the full array is a Quad Bayer's (2x2 of a colour): only its binned modes are Bayer */
};
enum {
	REGS_SONY,                      /* SMIA's registers (frame length 0x0340, coarse integration 0x0202, analog gain 0x0204, hold
	                                 * 0x0104), the gain register Sony's: 1024 - 1024 / gain (the program's gain is linear,
	                                 * gain_unity = 1x, and is turned into that here) */
};
#define SONY_GAIN_UNITY         256
#define SONY_GAIN_CODE_MAX      960     /* 16x, the module files' maximum analog gain */
static const struct sensor_hw sensors[] = {
	{       /* cam-sensor1: IMX376K, CCI master 1 (GPIO 19/20), reset GPIO 23, MCLK2 (GPIO 15), CSIPHY 1 */
		.name = "second (IMX376K)", .tab = "camera-imx376k.tab", .bus_name = "CCI master 1", .sda = 19, .scl = 20, .reset = 23,
		.mclk_cmd = 0x4044, .mclk_cbcr = 0x405c, .mclk_pin = 15,
		.vana = { SUPPLY_GPIO, 2800, 78, 200, NULL }, .vdig = { SUPPLY_GPIO, 1100, 79, 200, NULL },
		.vio = { SUPPLY_RPMH, 1800, 0, 0, "vsa1" },
		.csiphy = 1, .phy_cbcr = 0x5044, .timer_cmd = 0x5028, .timer_cbcr = 0x5040, .regs = REGS_SONY, .bayer = 0,
		.pattern_bayer = 0, .chip = "IMX376K", .gain_unity = SONY_GAIN_UNITY, .gain_max = 16 * SONY_GAIN_UNITY,
		.vaf = { SUPPLY_GPIO, 2800, 77, 200, NULL }, .af_address = 0x74, .af_sa_address = 0x0d,
	},
	{       /* cam-sensor0: IMX519, CCI master 0 (GPIO 17/18), reset GPIO 28, MCLK0 (GPIO 13), CSIPHY 0 (C-PHY) */
		.name = "main (IMX519)", .tab = "camera-imx519.tab", .bus_name = "CCI master 0", .sda = 17, .scl = 18, .reset = 28,
		.mclk_cmd = 0x4004, .mclk_cbcr = 0x401c, .mclk_pin = 13,
		.vana = { SUPPLY_GPIO, 2800, 27, 200, NULL }, .vdig = { SUPPLY_GPIO, 1100, 8, 200, NULL },
		.vio = { SUPPLY_RPMH, 1800, 0, 0, "vsa1" },
		.csiphy = 0, .phy_cbcr = 0x5020, .timer_cmd = 0x5004, .timer_cbcr = 0x501c, .regs = REGS_SONY, .bayer = 0,
		.pattern_bayer = 0, .chip = "IMX519", .gain_unity = SONY_GAIN_UNITY, .gain_max = 16 * SONY_GAIN_UNITY,
		.vaf = { SUPPLY_GPIO, 2800, 31, 200, NULL }, .af_address = 0x72, .af_sa_address = 0x0c,
	},
	{       /* cam-sensor2: IMX371, CCI master 0 (GPIO 17/18), reset GPIO 9, MCLK1 (GPIO 14), CSIPHY 2 */
		.name = "front (IMX371)", .tab = "camera-imx371.tab", .bus_name = "CCI master 0", .sda = 17, .scl = 18, .reset = 9,
		.mclk_cmd = 0x4024, .mclk_cbcr = 0x403c, .mclk_pin = 14,
		.vana = { SUPPLY_GPIO, 2800, 104, 200, NULL }, .vdig = { SUPPLY_GPIO, 1100, 117, 200, NULL },
		.vio = { SUPPLY_RPMH, 1800, 0, 0, "vsa1" },
		.csiphy = 2, .phy_cbcr = 0x5068, .timer_cmd = 0x504c, .timer_cbcr = 0x5064, .regs = REGS_SONY, .bayer = 0,
		.pattern_bayer = 0, .chip = "IMX371", .gain_unity = SONY_GAIN_UNITY, .gain_max = 16 * SONY_GAIN_UNITY,
		.quad = true,           /* (r189: its 4608x3456 is RRGG/RRGG/GGBB/GGBB, though its module file says RGGB) */
	},
};
static const struct sensor_hw *hw = &sensors[1];
static unsigned hw_index = 1;

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("camera: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* The pins written while powered up, as they were found, to be put back: the reset pin, MCLK's, the two supplies'. */
static uint32_t found_reset_cfg, found_reset_io, found_mclk_cfg;
static bool pins_found;

/* the sensor's reset and MCLK pins as they are now (to be put back when it is powered down) */
static void
find_pins(void)
{
	found_reset_cfg = rd(GPIO_CFG(hw->reset));
	found_reset_io = rd(GPIO_IO(hw->reset));
	found_mclk_cfg = rd(GPIO_CFG(hw->mclk_pin));
	pins_found = true;
}

static void
reset_pin(bool high)
{
	wr(GPIO_IO(hw->reset), high ? 2 : 0);
	wr(GPIO_CFG(hw->reset), CFG_OUTPUT | CFG_DRIVE_MA(2));         /* GPIO, no pull, 2 mA: Android's cam_sensor_rear_0_rest_active */
}

/* ---- I2C by hand (as in hakios.haptic.c) ---------------------------------------------------------- */

struct bus {
	const char *    name;
	unsigned        sda, scl;
	uint32_t        cfg[2], io[2];  /* the pins as they were found, while taken */
};
static struct bus cci_bus;              /* the sensor's (from hw) */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(struct bus *b)
{
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	line(b->sda, false);
	half_bit();
	line(b->scl, false);
	half_bit();
}

static void
i2c_stop(struct bus *b)
{
	line(b->sda, false);
	half_bit();
	line(b->scl, true);
	half_bit();
	line(b->sda, true);
	half_bit();
}

static bool
i2c_bit(struct bus *b, bool out)
{
	bool in;

	line(b->sda, out);
	half_bit();
	line(b->scl, true);
	for (unsigned stretched = 0; !level_of(b->scl) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(b->sda);
	line(b->scl, false);
	return in;
}

static bool
i2c_write(struct bus *b, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit(b, (byte >> bit) & 1);
	}
	return !i2c_bit(b, true);
}

static uint8_t
i2c_read(struct bus *b, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(b, true));
	}
	i2c_bit(b, !more);
	return byte;
}

static void
bus_give_back(struct bus *b)
{
	wr(GPIO_IO(b->sda), b->io[0] & 2);
	wr(GPIO_IO(b->scl), b->io[1] & 2);
	wr(GPIO_CFG(b->sda), b->cfg[0]);
	wr(GPIO_CFG(b->scl), b->cfg[1]);
}

/*
 * A sensor stopped in the middle of a byte -- the camera switched away from it between frames, its
 * digital rail left on because the driver found it on -- goes on holding the data line down, and the
 * bus never comes to rest again however long one waits for it. The way out is the one Linux's
 * bit-banged master takes (i2c_generic_scl_recovery): clock the line that is free until the sensor has
 * finished that byte and lets the data line go, then a stop. Nine clocks are a byte and the
 * acknowledgement after it. This is what kept the wide camera's bus (CCI 0 master 1) for the rest of a
 * session once Camera.app had switched to the front camera and back.
 */
static bool
bus_unwedge(struct bus *b)
{
	for (unsigned clocks = 1; clocks <= 9; clocks++) {
		line(b->scl, false);
		half_bit();
		line(b->scl, true);
		half_bit();
		if (level_of(b->sda)) {
			i2c_stop(b);
			say("camera: %s: the data line was held down and came back after %u clock(s)\n", b->name, clocks);
			return true;
		}
	}
	return false;
}

static bool
bus_take(struct bus *b)
{
	b->cfg[0] = rd(GPIO_CFG(b->sda));
	b->cfg[1] = rd(GPIO_CFG(b->scl));
	b->io[0] = rd(GPIO_IO(b->sda));
	b->io[1] = rd(GPIO_IO(b->scl));
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	if (level_of(b->sda) && level_of(b->scl)) {
		return true;
	}
	/* the data line alone held down is a sensor that did not finish a byte: clock it out (see above) */
	if (level_of(b->scl) && !level_of(b->sda) && bus_unwedge(b) && level_of(b->scl)) {
		return true;
	}
	{
		bool sda = level_of(b->sda), scl = level_of(b->scl);

		bus_give_back(b);
		say("camera: %s is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", b->name, b->sda, sda, b->scl, scl);
	}
	return false;
}

/*
 * A transaction: `out' (the register's address, and for a write the data) written, then for a read
 * `in_count' bytes read after a repeated start. False if anybody did not acknowledge.
 */
static bool
i2c_transfer(struct bus *b, uint8_t address, const uint8_t *out, unsigned out_count, uint8_t *in, unsigned in_count)
{
	bool answered;

	if (!bus_take(b)) {
		return false;
	}
	i2c_start(b);
	answered = i2c_write(b, (uint8_t)(address << 1));
	for (unsigned i = 0; answered && i < out_count; i++) {
		answered = i2c_write(b, out[i]);
	}
	if (answered && in_count != 0) {
		i2c_start(b);
		answered = i2c_write(b, (uint8_t)(address << 1 | 1));
		for (unsigned i = 0; answered && i < in_count; i++) {
			in[i] = i2c_read(b, i + 1 < in_count);
		}
	}
	i2c_stop(b);
	bus_give_back(b);
	return answered;
}

/* ---- the PMIC arbiter (version 5), read only, through its observer: as in hakios.torch8998.c ---------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000

#define PID_LVS1                0x180   /* the PM8998's regulators (slave 1, where its LDOs answer: hakios.mpss845.c), LVS1 at 0x8000: VIO */
#define PID_BOB                 0x3a0   /* the PMI8998 (slave 3), BOB at 0xa000: what VANA and VDIG's regulators are fed from */
#define PID_L1                  0x140   /* the PM8998's L1 at 0x4000 (slave 1): the CSIPHYs' mipi-csi-vdd (0.88 V; the display's DSI PHY's too) */
#define REFGEN_PHYS             0x00ff1000UL    /* the CSIPHYs' (and the DSI PHY's) reference generator: looked at only */
#define REG_STATUS1             0x08    /* [7] ready (PMIC4 regulators) */
#define REG_VSET_LB             0x40
#define REG_MODE                0x45
#define REG_EN_CTL              0x46    /* [7] enabled */

static vm_offset_t arb_core, arb_observer;

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
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? "the transfer was dropped" : NULL;
}

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

/* A PMIC peripheral's registers, read through the arbiter's observer (any channel that is that peripheral's). */
static bool
pmic_peek(uint16_t id, uint8_t reg, uint8_t *values, unsigned count)
{
	if (arb_core == 0) {
		return false;
	}
	for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		for (unsigned i = 0; i < count; i++) {
			vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * n;

			lock();
			wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)(reg + i) << 4);
			const char *problem = wait_done(channel);
			values[i] = (uint8_t)rd(channel + OBSERVER_DATA0);
			unlock();
			if (problem != NULL) {
				return false;
			}
		}
		return true;
	}
	return false;
}

/* a regulator of the PMICs: ready, enabled, its set point and mode (reading only) */
static bool
show_regulator(const char *what, uint16_t id, bool *ready)
{
	uint8_t head[2] = { 0, 0 }, status = 0, vset[2] = { 0, 0 }, mode = 0, enable = 0;

	*ready = false;
	if (!pmic_peek(id, 0x04, head, 2) || !pmic_peek(id, REG_STATUS1, &status, 1) || !pmic_peek(id, REG_VSET_LB, vset, 2) ||
	    !pmic_peek(id, REG_MODE, &mode, 1) || !pmic_peek(id, REG_EN_CTL, &enable, 1)) {
		say("camera: %s (peripheral %#x): cannot be read\n", what, id);
		return false;
	}
	*ready = (status & 0x80) != 0;
	say("camera: %s (peripheral %#x): type %#04x subtype %#04x, status %#04x (%s), enable %#04x, set point %u mV, mode %#04x\n", what,
	    id, head[0], head[1], status, *ready ? "ready" : "not ready", enable, vset[1] << 8 | vset[0], mode);
	return true;
}

/* ---- RPMh: the command DB, and DRV 2's second TCS for requests made at once ------------------------- */

#define CMD_DB_PHYS             0x85fe0000UL    /* SDM845's (reserved memory "cmd_db", as hakios.a630.c finds it) */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x179e0000UL    /* DRV 2's (dt: rsc@179c0000, drv-id 2) */
#define RSC_TCS_BASE            0xd00
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)
#define RSC_TCS_COUNT           9               /* dt qcom,tcs-config: 2 active, 3 sleep, 3 wake, 1 control */
#define CAMERA_TCS              1               /* the second for requests made at once (the modem's, the audio DSP's and the
                                                 * DDR vote's drivers take the first, one at a time) */
#define VRM_VOLTAGE             0x0             /* an RPMh VRM resource's registers (rpmh-regulator.c) */
#define VRM_ENABLE              0x4
#define VRM_MODE                0x8
#define BOB_MV                  3312            /* the device tree's (rgltr-min-voltage of the sensors' VANA and VDIG) */
#define BOB_MODE_AUTO           2               /* PMIC4 BOB: 0 pass, 1 PFM, 2 auto, 3 PWM */

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

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern lck_mtx_t hakios_rsc_lock;       /* RPMh's sequencer, shared with the modem's and the audio DSP's drivers */
static const struct cmd_db_header *cmd_db;
static vm_offset_t rsc;

/* RPMh's address for a resource, and its levels (as many as `max'); 0: not there. */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned max, unsigned *count)
{
	char id[8] = { 0 };

	*count = 0;
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
			const uint8_t *data;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return entry[i].address;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			for (unsigned l = 0; l < entry[i].length / 2u && l < max; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

/* what DRV 2's sequencer holds for a resource's registers (address .. address + 0xc), in any of its TCSs */
static void
show_votes(const char *name, uint32_t address)
{
	for (unsigned t = 0; address != 0 && t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = rd(rsc + RSC_TCS(t) + TCS_ENABLED);

		for (unsigned c = 0; c < 16; c++) {
			uint32_t where = rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 4);

			if (where >= address && where < address + 0x10) {
				say("camera:   %s: TCS %u command %u: %#x <- %u, %senabled, message id %#x, status %#x\n", name, t, c, where,
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 8), (enabled >> c) & 1 ? "" : "not ",
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c)), rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 12));
			}
		}
	}
}

static void
show_rail(const char *name, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;
	char line[200];
	int at;

	*address = rpmh_resource(name, levels, 16, &count);
	at = snprintf(line, sizeof(line), "camera: RPMh's %s at %#x, levels:", name, *address);
	for (unsigned l = 0; l < count && at < (int)sizeof(line) - 8; l++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", levels[l]);
	}
	say("%s\n", line);
	show_votes(name, *address);
}

/* Up to three writes to RPMh's resources through the camera's TCS, waited for, hakios_rsc_lock held. True: RPMh has said done. */
static bool
rpmh_write_locked(unsigned n, const uint32_t *addresses, const uint32_t *values)
{
	vm_offset_t tcs = rsc + RSC_TCS(CAMERA_TCS);
	uint32_t done = 0, all = (1u << n) - 1;

	if (rsc == 0) {
		return false;
	}
	if (rd(tcs + TCS_STATUS) == 0) {
		say("camera: TCS %u of the sequencer is busy (control %#x)\n", CAMERA_TCS, rd(tcs + TCS_CONTROL));
		return false;
	}
	wr(rsc + RSC_IRQ_CLEAR, 1u << CAMERA_TCS);
	for (unsigned r = 0; r < n; r++) {
		wr(tcs + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(tcs + TCS_COMMAND(r) + 4, addresses[r]);
		wr(tcs + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(tcs + TCS_WAIT_FOR_DONE, all);
	wr(tcs + TCS_ENABLED, all);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) | TCS_TRIGGER);
	/* (each command's status: [8] issued, [16] completed -- the TCS's interrupt may not be enabled for this one) */
	for (unsigned waited = 0; waited < 100000 && !done; waited += 50) {
		unsigned completed = 0;

		for (unsigned r = 0; r < n; r++) {
			completed += (rd(tcs + TCS_COMMAND(r) + 12) >> 16) & 1;
		}
		done = (rd(rsc + RSC_IRQ_STATUS) & (1u << CAMERA_TCS)) || completed == n;
		if (!done) {
			spin_us(50);
		}
	}
	wr(rsc + RSC_IRQ_CLEAR, 1u << CAMERA_TCS);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(tcs + TCS_ENABLED, 0);
	return done != 0;
}

/* the same, taking the sequencer's lock (as the modem's and the audio DSP's drivers take it; not on the way out of a panic) */
static bool
rpmh_write(unsigned n, const uint32_t *addresses, const uint32_t *values)
{
	bool done;

	if (!leaving) {
		lck_mtx_lock(&hakios_rsc_lock);
	}
	done = rpmh_write_locked(n, addresses, values);
	if (!leaving) {
		lck_mtx_unlock(&hakios_rsc_lock);
	}
	return done;
}

/*
 * The PM8998's LVS1 (VIO, "vsa1") switched on or given back; and BOB ("bobb1") at the device tree's 3.312 V in auto
 * mode, when it was not on already. Each vote is recorded before it is asked for, as rails() does: one RPMh finishes
 * after our wait must still be given back.
 */
static bool vio_voted, bob_voted;
static bool rails_get(void);            /* (below, with the camera subsystem's clocks) */
static void rails_put(bool all);

static bool
vrm_vote(const char *name, bool on, bool *voted)
{
	uint16_t levels[1];
	unsigned count;
	uint32_t base = rpmh_resource(name, levels, 1, &count), addresses[3], values[3];
	bool bob = strcmp(name, "bobb1") == 0, done;
	unsigned n = 0;

	if (base == 0) {
		say("camera: RPMh has no %s\n", name);
		return false;
	}
	if (on && bob) {
		addresses[n] = base + VRM_VOLTAGE, values[n++] = BOB_MV;
		addresses[n] = base + VRM_MODE, values[n++] = BOB_MODE_AUTO;
	}
	addresses[n] = base + VRM_ENABLE, values[n++] = on;
	*voted = on;
	done = rpmh_write(n, addresses, values);
	say("camera: RPMh %s %s (%#x) %s\n", done ? "took" : "DID NOT SAY IT TOOK", name, base, on ? "on" : "off (given back)");
	return done;
}

/* ---- the camera's clocks: camcc (camcc-sdm845.c) ----------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_CAMERA_AHB_CBCR     0xb008          /* always on (gcc-sdm845.c: critical) */
#define GCC_CAMERA_AXI_CBCR     0xb020
#define GCC_CAMERA_XO_CBCR      0xb02c          /* always on in Linux (gcc-sdm845.c: CLK_IS_CRITICAL), but found off here: camcc's crystal,
                                                 * which its power domains' state machines run on */
#define TITAN_TOP_BCR           0xb130          /* the titan top block's reset (Android's camcc-sdm845.c) */
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x10000UL
#define MCLK_CMD_RCGR           (hw->mclk_cmd)          /* [0] update, [31] the root is off */
#define MCLK_CFG_RCGR           (hw->mclk_cmd + 4)      /* [4:0] divider (2n - 1), [10:8] source (0: the crystal), [13:12] M/N mode */
#define MCLK_M                  (hw->mclk_cmd + 8)
#define MCLK_N                  (hw->mclk_cmd + 12)
#define MCLK_D                  (hw->mclk_cmd + 16)
#define MCLK_CBCR               (hw->mclk_cbcr)         /* [0] enable, [31] the clock is off */
#define TITAN_TOP_GDSCR         0xb134          /* [0] collapse, [31] on */
#define CBCR_OFF                (1u << 31)
#define CMD_UPDATE              1u
#define GDSC_COLLAPSE           1u
#define GDSC_ON                 (1u << 31)
#define GDSC_CFG_UP             (1u << 16)      /* CFG_GDSCR (GDSCR + 4): power-up complete */
#define GDSC_CFG_DOWN           (1u << 15)      /* ... power-down complete */
#define SRC_XO                  0               /* camcc-sdm845.c's cam_cc_parent_map_0 */
#define SRC_PLL2_EVEN           1
#define SRC_PLL3_EVEN           5

static vm_offset_t gcc, camcc;
static uint32_t found_mclk_cfg_rcgr, found_mclk_cbcr, found_gdscr, found_mclk_mnd[3];
static bool gdsc_switched, mclk_switched, mclk_pll, gcc_axi_switched, gcc_xo_switched;

/*
 * camcc's PLLs used here (Fabia, clk-alpha-pll.c's layout), configured as camcc-sdm845.c's probe does and started as
 * alpha_pll_fabia_enable() does: PLL2 at 960 MHz (L 0x32), its even output at 480 (post divider 2) -- MCLK's 24 MHz
 * (/ 10, M/N 1/2), the CSIPHY timer's 240 (/ 2), slow_ahb's 80 (/ 6) --, and PLL3 at 384 MHz (L 0x14), its even output
 * undivided -- cphy_rx, ife_lite and ife_lite_csid at 384. Each started by the first that needs it, taken down by the
 * last (and only if it was found stopped: somebody else's is left alone).
 */
#define PLL_MODE                0x00            /* [0] outputs, [2] out of reset, [20] voted (FSM), [23] update bypass, [31] locked */
#define PLL_L_VAL               0x04
#define PLL_USER_CTL            0x0c            /* [0] main, [1] even, [2] odd outputs; [11:8] the even output's post divider */
#define PLL_OPMODE              0x2c            /* 0 standby, 1 run */
#define PLL_FRAC                0x38
static struct pll {
	const char *    name;
	uint32_t        base, l, even_div;      /* (even_div: the post-divider field, 0 for 1, 1 for 2) */
	unsigned        users;
	bool            ours;
	uint32_t        found_user_ctl, found_l, found_frac, found_mode;
} plls[] = {
	{ "PLL2", 0x2000, 0x32, 1, 0, false, 0, 0, 0, 0 },
	{ "PLL3", 0x3000, 0x14, 0, 0, false, 0, 0, 0, 0 },
};
enum { PLL2, PLL3 };

static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check_mask)
{
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check_mask) != (value & check_mask)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_bits(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(camcc + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

static void
show_clocks(void)
{
	say("camera: GCC: camera AHB %#x, AXI %#x, XO %#x\n", rd(gcc + GCC_CAMERA_AHB_CBCR), rd(gcc + GCC_CAMERA_AXI_CBCR),
	    rd(gcc + GCC_CAMERA_XO_CBCR));
	say("camera: camcc: titan top GDSC %#x (cfg %#x, the block's reset %#x); the sensor's MCLK RCG cmd %#x cfg %#x m %#x n %#x d %#x, branch %#x\n",
	    rd(camcc + TITAN_TOP_GDSCR), rd(camcc + TITAN_TOP_GDSCR + 4), rd(camcc + TITAN_TOP_BCR),
	    rd(camcc + MCLK_CMD_RCGR), rd(camcc + MCLK_CFG_RCGR), rd(camcc + MCLK_M), rd(camcc + MCLK_N), rd(camcc + MCLK_D),
	    rd(camcc + MCLK_CBCR));
}

static void
show_plls(void)
{
	for (unsigned p = 0; p < 4; p++) {
		vm_offset_t pll = camcc + 0x1000UL * p;

		say("camera: camcc PLL%u: mode %#x, l %#x, user_ctl %#x, config_ctl %#x %#x, test_ctl %#x %#x, opmode %#x, status %#x, frac %#x\n",
		    p, rd(pll + PLL_MODE), rd(pll + PLL_L_VAL), rd(pll + PLL_USER_CTL), rd(pll + 0x14), rd(pll + 0x18), rd(pll + 0x1c),
		    rd(pll + 0x20), rd(pll + PLL_OPMODE), rd(pll + 0x24), rd(pll + PLL_FRAC));
	}
}

static bool
gdsc_on(void)
{
	found_gdscr = rd(camcc + TITAN_TOP_GDSCR);
	if (!(found_gdscr & GDSC_COLLAPSE) && (found_gdscr & GDSC_ON)) {
		say("camera: the titan top power domain was on already\n");
		return true;
	}
	if (!(rd(gcc + GCC_CAMERA_XO_CBCR) & 1)) {
		/* (camcc's crystal: without it the GDSC never says it is up) */
		wr(gcc + GCC_CAMERA_XO_CBCR, rd(gcc + GCC_CAMERA_XO_CBCR) | 1);
		gcc_xo_switched = true;
		for (unsigned waited = 0; (rd(gcc + GCC_CAMERA_XO_CBCR) & CBCR_OFF) && waited < 500; waited += 10) {
			spin_us(10);
		}
		say("camera: gcc camera_xo on (%#x)\n", rd(gcc + GCC_CAMERA_XO_CBCR));
	}
	if (!(rd(gcc + GCC_CAMERA_AXI_CBCR) & 1)) {
		/* (the domain's bus clock first, as gcc's camera_axi is the GDSC's parent in Android's tree) */
		wr(gcc + GCC_CAMERA_AXI_CBCR, rd(gcc + GCC_CAMERA_AXI_CBCR) | 1);
		gcc_axi_switched = true;
		say("camera: gcc camera_axi on (%#x)\n", rd(gcc + GCC_CAMERA_AXI_CBCR));
	}
	if (!camcc_put(TITAN_TOP_GDSCR, found_gdscr & ~GDSC_COLLAPSE, GDSC_COLLAPSE)) {
		return false;
	}
	gdsc_switched = true;
	spin_us(1);
	if (!wait_bits(TITAN_TOP_GDSCR + 4, GDSC_CFG_UP, GDSC_CFG_UP, 500) && !wait_bits(TITAN_TOP_GDSCR, GDSC_ON, GDSC_ON, 100)) {
		say("camera: the titan top power domain does not come on (gdscr %#x, cfg_gdscr %#x, the block's reset %#x)\n",
		    rd(camcc + TITAN_TOP_GDSCR), rd(camcc + TITAN_TOP_GDSCR + 4), rd(camcc + TITAN_TOP_BCR));
		give_up("a power domain that does not come on");
		return false;
	}
	say("camera: the titan top power domain on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
	return true;
}

static void
gdsc_off(void)
{
	if (gdsc_switched) {
		wr(camcc + TITAN_TOP_GDSCR, rd(camcc + TITAN_TOP_GDSCR) | GDSC_COLLAPSE);
		gdsc_switched = false;
		say("camera: the titan top power domain %s (%#x)\n", wait_bits(TITAN_TOP_GDSCR + 4, GDSC_CFG_DOWN, GDSC_CFG_DOWN, 500) ||
		    wait_bits(TITAN_TOP_GDSCR, GDSC_ON, 0, 100) ? "off" : "NOT OFF", rd(camcc + TITAN_TOP_GDSCR));
	}
	if (gcc_axi_switched) {
		wr(gcc + GCC_CAMERA_AXI_CBCR, rd(gcc + GCC_CAMERA_AXI_CBCR) & ~1u);
		gcc_axi_switched = false;
		say("camera: gcc camera_axi back off (%#x)\n", rd(gcc + GCC_CAMERA_AXI_CBCR));
	}
	if (gcc_xo_switched) {
		wr(gcc + GCC_CAMERA_XO_CBCR, rd(gcc + GCC_CAMERA_XO_CBCR) & ~1u);
		gcc_xo_switched = false;
		say("camera: gcc camera_xo back off (%#x)\n", rd(gcc + GCC_CAMERA_XO_CBCR));
	}
}

/* clk_fabia_pll_configure() with camcc-sdm845.c's values (L, no alpha, nothing else), then alpha_pll_fabia_enable() */
static bool
pll_up(unsigned which)
{
	struct pll *p = &plls[which];
	uint32_t mode = rd(camcc + p->base + PLL_MODE);

	if (p->users++ != 0) {
		return true;
	}
	if (mode & (1u << 20)) {
		give_up("a camcc PLL in FSM (voted) mode: somebody else's");
		return false;
	}
	if ((mode & 1) && (rd(camcc + p->base + PLL_OPMODE) & 1)) {
		/* (running already: used as it is, its rate checked, left running at the end) */
		if (rd(camcc + p->base + PLL_L_VAL) != p->l || (rd(camcc + p->base + PLL_USER_CTL) >> 8 & 0xf) != p->even_div) {
			say("camera: camcc's %s runs already at another rate (l %#x, user_ctl %#x)\n", p->name, rd(camcc + p->base + PLL_L_VAL),
			    rd(camcc + p->base + PLL_USER_CTL));
			give_up("a camcc PLL somebody else runs at another rate");
			return false;
		}
		say("camera: camcc's %s was running already\n", p->name);
		return true;
	}
	p->found_user_ctl = rd(camcc + p->base + PLL_USER_CTL);
	p->found_l = rd(camcc + p->base + PLL_L_VAL);
	p->found_frac = rd(camcc + p->base + PLL_FRAC);
	p->found_mode = mode;
	p->ours = true;
	if (!camcc_put(p->base + PLL_L_VAL, p->l, ~0u) || !camcc_put(p->base + PLL_FRAC, 0, 0xffff) ||
	    !camcc_put(p->base + PLL_USER_CTL, (p->found_user_ctl & ~0xf07u) | p->even_div << 8, 0xf07) ||
	    !camcc_put(p->base + PLL_MODE, rd(camcc + p->base + PLL_MODE) | 1u << 23, 1u << 23) ||
	    !camcc_put(p->base + PLL_MODE, rd(camcc + p->base + PLL_MODE) & ~1u, 1) || !camcc_put(p->base + PLL_OPMODE, 0, 1) ||
	    !camcc_put(p->base + PLL_MODE, rd(camcc + p->base + PLL_MODE) | 1u << 2, 1u << 2) || !camcc_put(p->base + PLL_OPMODE, 1, 1)) {
		return false;
	}
	if (!wait_bits(p->base + PLL_MODE, 1u << 31, 1u << 31, 1000)) {
		say("camera: camcc's %s does not lock (mode %#x)\n", p->name, rd(camcc + p->base + PLL_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_put(p->base + PLL_USER_CTL, rd(camcc + p->base + PLL_USER_CTL) | 3, 3) ||
	    !camcc_put(p->base + PLL_MODE, rd(camcc + p->base + PLL_MODE) | 1, 1)) {
		return false;
	}
	say("camera: camcc's %s locked at %u MHz (mode %#x, user_ctl %#x)\n", p->name, p->l * 192 / 10, rd(camcc + p->base + PLL_MODE),
	    rd(camcc + p->base + PLL_USER_CTL));
	return true;
}

/* alpha_pll_fabia_disable(), then back into reset as it was found */
static void
pll_down(unsigned which, bool all)
{
	struct pll *p = &plls[which];

	if (p->users == 0 || (--p->users != 0 && !all)) {
		return;
	}
	p->users = 0;
	if (!p->ours) {
		return;
	}
	wr(camcc + p->base + PLL_MODE, rd(camcc + p->base + PLL_MODE) & ~1u);
	wr(camcc + p->base + PLL_USER_CTL, p->found_user_ctl);
	wr(camcc + p->base + PLL_OPMODE, 0);
	/* (out of reset and the update bypass as they were found; then its rate's registers) */
	wr(camcc + p->base + PLL_MODE, (rd(camcc + p->base + PLL_MODE) & ~(1u << 2 | 1u << 23)) | (p->found_mode & (1u << 2 | 1u << 23)));
	wr(camcc + p->base + PLL_L_VAL, p->found_l);
	wr(camcc + p->base + PLL_FRAC, p->found_frac);
	p->ours = false;
	say("camera: camcc's %s off (mode %#x)\n", p->name, rd(camcc + p->base + PLL_MODE));
}

/* MCLK at 24 MHz (PLL2's even output / 10, M/N 1/2: ftbl_cam_cc_mclk0_clk_src) or at 19.2 MHz from the crystal itself */
static bool
mclk_on(uint32_t khz)
{
	bool pll = khz == 24000;

	found_mclk_cfg_rcgr = rd(camcc + MCLK_CFG_RCGR);
	found_mclk_cbcr = rd(camcc + MCLK_CBCR);
	found_mclk_mnd[0] = rd(camcc + MCLK_M);
	found_mclk_mnd[1] = rd(camcc + MCLK_N);
	found_mclk_mnd[2] = rd(camcc + MCLK_D);
	if (found_mclk_cbcr & 1) {
		give_up("MCLK is on already (somebody else's)");
		return false;
	}
	mclk_switched = true;
	if (pll) {
		mclk_pll = true;
		/* __clk_rcg2_configure(): M 1, N as ~(n - m), D as ~2d (2d = n), then source, divider 2 * 10 - 1, dual-edge M/N */
		if (!pll_up(PLL2) || !camcc_put(MCLK_M, (found_mclk_mnd[0] & ~0xffu) | 1, 0xff) ||
		    !camcc_put(MCLK_N, (found_mclk_mnd[1] & ~0xffu) | (~(2u - 1) & 0xff), 0xff) ||
		    !camcc_put(MCLK_D, (found_mclk_mnd[2] & ~0xffu) | (~2u & 0xff), 0xff)) {
			return false;
		}
	}
	if (!camcc_put(MCLK_CFG_RCGR, (found_mclk_cfg_rcgr & ~0x371fu) | (pll ? (uint32_t)SRC_PLL2_EVEN << 8 | 2u << 12 | 19u :
	    (uint32_t)SRC_XO << 8 | 1u), 0x371f) || !camcc_put(MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE, 0)) {
		return false;
	}
	if (!wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500)) {
		say("camera: MCLK's RCG does not take its configuration (cmd %#x)\n", rd(camcc + MCLK_CMD_RCGR));
		give_up("an RCG that does not update");
		return false;
	}
	if (!camcc_put(MCLK_CBCR, found_mclk_cbcr | 1, 1)) {
		return false;
	}
	if (!wait_bits(MCLK_CBCR, CBCR_OFF, 0, 500)) {
		say("camera: MCLK's branch does not come on (%#x)\n", rd(camcc + MCLK_CBCR));
		give_up("a clock that does not come on");
		return false;
	}
	wr(GPIO_CFG(hw->mclk_pin), CFG_FUNCTION(1) | CFG_DRIVE_MA(2));    /* cam_mclk, no pull, 2 mA: cam_sensor_*_mclk_active */
	say("camera: MCLK on at %u.%u MHz (RCG cfg %#x, branch %#x)\n", khz / 1000, khz % 1000 / 100, rd(camcc + MCLK_CFG_RCGR),
	    rd(camcc + MCLK_CBCR));
	return true;
}

static void
mclk_off(void)
{
	if (!mclk_switched) {
		return;
	}
	if (pins_found) {
		wr(GPIO_CFG(hw->mclk_pin), found_mclk_cfg);
	}
	wr(camcc + MCLK_CBCR, rd(camcc + MCLK_CBCR) & ~1u);
	(void)wait_bits(MCLK_CBCR, CBCR_OFF, CBCR_OFF, 500);
	/* (M, N, D and the configuration as found, then one update: what is written after an update stays marked dirty) */
	wr(camcc + MCLK_M, found_mclk_mnd[0]);
	wr(camcc + MCLK_N, found_mclk_mnd[1]);
	wr(camcc + MCLK_D, found_mclk_mnd[2]);
	wr(camcc + MCLK_CFG_RCGR, found_mclk_cfg_rcgr);
	wr(camcc + MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE);
	(void)wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500);
	if (mclk_pll) {
		pll_down(PLL2, false);
		mclk_pll = false;
	}
	mclk_switched = false;
	say("camera: MCLK off (branch %#x, RCG cfg %#x as found)\n", rd(camcc + MCLK_CBCR), rd(camcc + MCLK_CFG_RCGR));
}

/* ---- the sensor's tables, its power and its ID ---------------------------------------------------------- */

/*
 * The sensor's tables are Android's, read at run time from fw/ of the small image, where
 * userland/build.sh puts what tools/camsensor.py makes of the sensor's module file ("RWCTAB02"):
 * the slave address and the chip ID's read, the lane assignment, the modes, the power sequences
 * (Android's cam_sensor kinds: 0 MCLK, 1 VANA, 2 VDIG, 3 VIO, 8 RESET, with their delays) and the
 * register tables (init, res0.., on, off, ...).
 */
struct ctab_head {
	char            magic[8];       /* "RWCTAB02" */
	uint32_t        slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes;
};
struct ctab_mode {
	uint32_t        width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};
struct ctab_entry {
	uint16_t        address, data;  /* (a power sequence's: kind, value -- MCLK in kHz) */
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay_us;
};

static const struct ctab_head *ctab;
static const struct ctab_mode *ctab_modes;

/* the sensor the rest works on: its descriptor, its CCI master's pins (its tables are loaded afresh: ctab_load()) */
static void
select_sensor(unsigned i)
{
	hw_index = i;
	hw = &sensors[i];
	cci_bus.name = hw->bus_name;
	cci_bus.sda = hw->sda;
	cci_bus.scl = hw->scl;
	ctab = NULL;
}
static const uint8_t *ctab_power, *ctab_tables;
static uint64_t ctab_size;

static bool
ctab_load(void)
{
	const uint8_t *at, *end;

	ctab = (const struct ctab_head *)hakios_image_file("fw", hw->tab, &ctab_size, NULL);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB02", 8) != 0 || ctab->modes == 0 ||
	    ctab->modes > 16) {
		say("camera: no %s (RWCTAB02) in fw/ of the small image (made from fw-camera/ when it is built)\n", hw->tab);
		ctab = NULL;
		return false;
	}
	end = (const uint8_t *)ctab + ctab_size;
	ctab_modes = (const struct ctab_mode *)(ctab + 1);
	ctab_power = (const uint8_t *)(ctab_modes + ctab->modes);
	at = ctab_power;
	for (int p = 0; p < 2; p++) {
		if (at + 12 > end) {
			ctab = NULL;
			return false;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct ctab_entry);
	}
	ctab_tables = at;
	if (ctab_tables + 4 > end) {
		ctab = NULL;
		return false;
	}
	say("camera: the %s sensor's tables: slave %#x, chip ID %#x at %#x, lanes %#x, %u modes, %u tables\n", hw->name, ctab->slave,
	    ctab->id, ctab->id_address, ctab->lane_assign, ctab->modes, *(const uint32_t *)ctab_tables);
	for (uint32_t m = 0; m < ctab->modes; m++) {
		const struct ctab_mode *o = &ctab_modes[m];

		say("camera:   mode %u: %ux%u at %u,%u, %u %s, settle %u, pixel clock %u, line %u, frame %u, %u.%03u frames/s\n", m, o->width,
		    o->height, o->x, o->y, o->lanes, o->cphy ? "trio(s), C-PHY" : "lane(s), D-PHY", o->settle, o->pixel_clock,
		    o->line_length, o->frame_length, o->fps_milli / 1000, o->fps_milli % 1000);
	}
	return true;
}

static const struct ctab_entry *
ctab_block(const uint8_t *at, uint32_t blocks, const char *name, uint32_t *count)
{
	const uint8_t *end = (const uint8_t *)ctab + ctab_size;

	for (uint32_t t = 0; t < blocks && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct ctab_entry) <= end) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	*count = 0;
	return NULL;
}

static const struct ctab_entry *
ctab_find(const char *name, uint32_t *count)
{
	return ctab == NULL ? NULL : ctab_block(ctab_tables + 4, *(const uint32_t *)ctab_tables, name, count);
}

static uint8_t
sensor_address(void)
{
	return (uint8_t)(ctab->slave >> 1);    /* (the module file's is the 8-bit one) */
}

static bool powered, reset_driven, power_rails;
static bool gpio_switched[4];           /* VANA's, VDIG's pin (0, 1), the lens motor's (3) */
static uint32_t found_supply_cfg[4], found_supply_io[4];

/* 1 ready, 0 not ready, -1 it cannot be read (and then nothing is voted for it: a vote somebody else holds could be replaced) */
static int
regulator_state(uint16_t id)
{
	uint8_t status = 0;

	if (!pmic_peek(id, REG_STATUS1, &status, 1)) {
		return -1;
	}
	return (status & 0x80) != 0;
}

/*
 * BOB, which VANA's and VDIG's regulators are fed from: on as the phone runs (Android's other users of it keep it so);
 * voted on here only if it is found off, and given back at the end.
 */
static bool
bob_up(void)
{
	int state = regulator_state(PID_BOB);

	if (state > 0) {
		return true;
	}
	if (state < 0) {
		give_up("BOB cannot be read: it is not voted for blind");
		return false;
	}
	say("camera: BOB is not ready: voted on\n");
	if (!vrm_vote("bobb1", true, &bob_voted)) {
		give_up("RPMh did not take BOB");
		return false;
	}
	wait_ms(2);
	if (regulator_state(PID_BOB) <= 0) {
		give_up("BOB does not come up");
		return false;
	}
	return true;
}

/* A supply on: a GPIO-switched regulator (the pin a high output: GPIO, no pull, 2 mA), or LVS1 through RPMh. */
static bool
supply_on(const struct supply *s, unsigned which)
{
	if (s->kind == SUPPLY_GPIO) {
		if (!bob_up()) {
			return false;
		}
		found_supply_cfg[which] = rd(GPIO_CFG(s->gpio));
		found_supply_io[which] = rd(GPIO_IO(s->gpio));
		gpio_switched[which] = true;
		wr(GPIO_IO(s->gpio), 2);
		wr(GPIO_CFG(s->gpio), CFG_OUTPUT | CFG_DRIVE_MA(2));
		if ((rd(GPIO_CFG(s->gpio)) & 0x3ff) != (CFG_OUTPUT | CFG_DRIVE_MA(2)) || !(rd(GPIO_IO(s->gpio)) & 2)) {
			give_up("a supply's pin that does not read back");
			return false;
		}
		spin_us(s->ramp_us);
		say("camera: gpio%u high: the %s regulator on\n", s->gpio, which == 0 ? "VANA" : which == 3 ? "lens motor's" : "VDIG");
		return true;
	}
	if (s->kind == SUPPLY_RPMH) {
		int state = regulator_state(PID_LVS1);

		if (state > 0) {
			say("camera: LVS1 (VIO) is on already: left as it is\n");
			return true;
		}
		if (state < 0) {
			give_up("LVS1 cannot be read: it is not voted for blind");
			return false;
		}
		if (!vrm_vote(s->rpmh, true, &vio_voted)) {
			give_up("RPMh did not take VIO");
			return false;
		}
		spin_us(200);
		if (regulator_state(PID_LVS1) <= 0) {
			give_up("VIO does not come up");
			return false;
		}
		return true;
	}
	give_up("a supply this sensor does not have");
	return false;
}

static void
supply_off(const struct supply *s, unsigned which)
{
	if (s->kind == SUPPLY_GPIO && which < 4 && which != 2 && gpio_switched[which]) {
		bool found_on = (found_supply_cfg[which] & CFG_OUTPUT) && (found_supply_io[which] & 2);

		if (!found_on) {
			wr(GPIO_IO(s->gpio), 0);
			spin_us(10);
		}
		wr(GPIO_IO(s->gpio), found_supply_io[which] & 2);
		wr(GPIO_CFG(s->gpio), found_supply_cfg[which]);
		gpio_switched[which] = false;
		say("camera: gpio%u back as it was found: the %s regulator off\n", s->gpio, which == 0 ? "VANA" : which == 3 ? "lens motor's" :
		    "VDIG");
	} else if (s->kind == SUPPLY_RPMH && vio_voted) {
		(void)vrm_vote(s->rpmh, false, &vio_voted);
	}
}

static void
sensor_delay(uint32_t us)
{
	if (us >= 1000) {
		wait_ms(us / 1000);
		us %= 1000;
	}
	spin_us(us);
}

/*
 * One step of the module file's power sequence (`up': part of the power-up, where a fault stops it). A supply's step
 * switches it on in the power-up and off in the power-down; the reset's value is the level; MCLK's is its rate (0 off).
 */
static bool
power_step(const struct ctab_entry *e, bool up)
{
	switch (e->address) {
	case 8:
		reset_driven = true;
		reset_pin(e->data != 0);
		break;
	case 0:
		if (e->data != 0 && e->data != 19200 && e->data != 24000) {
			give_up("an MCLK rate not known here");
			return false;
		}
		if (up && e->data != 0) {
			if (!gdsc_on() || !mclk_on(e->data)) {
				return false;
			}
		} else {
			mclk_off();
		}
		break;
	case 1: case 2: case 3: {
		const struct supply *s = e->address == 1 ? &hw->vana : e->address == 2 ? &hw->vdig : &hw->vio;
		unsigned which = e->address == 1 ? 0 : e->address == 2 ? 1 : 2;

		if (s->kind == SUPPLY_NONE) {
			give_up("a supply the device tree does not give this sensor");
			return false;
		}
		if (up) {
			if (!supply_on(s, which)) {
				return false;
			}
		} else {
			supply_off(s, which);
		}
		break;
	}
	default:
		say("camera: a power step of kind %u, which this does not know\n", e->address);
		give_up("a power sequence not understood");
		return false;
	}
	/* (capped at 100 ms, as the sensor tables' delays are: a corrupt table must not keep the rails up or spin for minutes) */
	sensor_delay(e->delay_us > 100000 ? 100000 : e->delay_us);
	return true;
}

/* Everything that was switched on, off again: the module file's power-down, then the rest as it was found. */
static void ois_down(void);            /* (the main camera's OIS: below, with its lens motor) */

static void
power_down(void)
{
	bool was = powered || mclk_switched || gdsc_switched || reset_driven || gpio_switched[0] || gpio_switched[1] || gpio_switched[3] ||
	    vio_voted ||
	    bob_voted || power_rails || gcc_axi_switched || gcc_xo_switched;
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrdn", &count) : NULL;

	if (!was) {
		return;
	}
	ois_down();                     /* (whatever path brought it here: the OIS before its sensor's supplies) */
	say("camera: powering down\n");
	for (uint32_t i = 0; e != NULL && i < count; i++) {
		(void)power_step(&e[i], false);
	}
	/* (whatever the sequence did not reach) */
	mclk_off();
	if (reset_driven) {
		reset_pin(false);
	}
	supply_off(&hw->vaf, 3);
	supply_off(&hw->vio, 2);
	supply_off(&hw->vdig, 1);
	supply_off(&hw->vana, 0);
	if (bob_voted) {
		(void)vrm_vote("bobb1", false, &bob_voted);
	}
	gdsc_off();
	if (reset_driven && pins_found) {
		wr(GPIO_IO(hw->reset), found_reset_io & 2);
		wr(GPIO_CFG(hw->reset), found_reset_cfg);
		reset_driven = false;
	}
	if (power_rails) {
		power_rails = false;
		rails_put(false);
	}
	powered = false;
	say("camera: powered down\n");
}

/* The module file's power-up, step by step. */
static bool
power_up(void)
{
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrup", &count) : NULL;

	if (e == NULL) {
		give_up("no power sequence");
		return false;
	}
	powered = true;
	if (!power_rails) {
		power_rails = true;
		if (!rails_get()) {
			give_up("RPMh did not take the camera's rails");
			return false;
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!power_step(&e[i], true)) {
			return false;
		}
	}
	say("camera: the %s sensor powered up (%u steps)\n", hw->name, count);
	return true;
}

static bool
read_chip_id(void)
{
	uint8_t where[2] = { (uint8_t)(ctab->id_address >> 8), (uint8_t)ctab->id_address }, id[4] = { 0, 0, 0, 0 };
	unsigned out = ctab->address_bytes == 2 ? 2 : 1, in = ctab->data_bytes >= 1 && ctab->data_bytes <= 4 ? ctab->data_bytes : 2;
	uint32_t chip = 0;

	/*
	 * The first word asked of the sensor after its supplies have come up. Powered up again within a
	 * run -- the capture graph stops and starts the stream as the application reconfigures its
	 * session -- it is not always ready by the time the power sequence's own waits are over, and one
	 * unanswered transfer used to end the camera for the rest of the run. It is asked again, for as
	 * long as the sensor's own data sheet would have it waiting.
	 */
	unsigned go;

	for (go = 0; go < 20; go++) {
		if (i2c_transfer(&cci_bus, sensor_address(), out == 2 ? where : where + 1, out, id, in)) {
			break;
		}
		wait_ms(2);
	}
	if (go == 20) {
		say("camera: the %s sensor does not answer at %#04x\n", hw->name, sensor_address());
		return false;
	}
	if (go != 0) {
		say("camera: the %s sensor answered at %#04x after %u more goes (%u ms)\n", hw->name, sensor_address(), go, go * 2);
	}
	for (unsigned i = 0; i < in; i++) {
		chip = chip << 8 | id[i];
	}
	say("camera: the %s sensor's chip ID: %#06x (%s)\n", hw->name, chip, (chip & ctab->id_mask) == (ctab->id & ctab->id_mask) ?
	    "as Android expects" : "NOT what Android expects");
	return (chip & ctab->id_mask) == (ctab->id & ctab->id_mask);
}

/*
 * While powered up: VIO and BOB still ready, VANA's and VDIG's pins still driving high. A regulator that is not ready
 * is asked again 2 ms on before it counts. (The SPMI reads are the observer's: nothing is written.)
 */
static bool
healthy(void)
{
	static const struct { uint16_t id; const char *name; } regulators[] = { { PID_LVS1, "LVS1 (VIO)" }, { PID_BOB, "BOB" } };

	for (unsigned r = 0; r < 2; r++) {
		uint8_t status = 0;
		bool ok = false;

		for (int look = 0; look < 2 && !ok; look++) {
			if (!pmic_peek(regulators[r].id, REG_STATUS1, &status, 1)) {
				return true;            /* (it cannot be read: not a fault of the camera's) */
			}
			ok = (status & 0x80) != 0;
			if (!ok && look == 0) {
				wait_ms(2);
			}
		}
		if (!ok && (r == 1 || hw->vio.kind == SUPPLY_RPMH)) {
			say("camera: %s is not ready (status %#04x)\n", regulators[r].name, status);
			give_up("a supply that went away");
			return false;
		}
	}
	for (unsigned which = 0; which < 2; which++) {
		const struct supply *s = which == 0 ? &hw->vana : &hw->vdig;

		if (gpio_switched[which] && !(rd(GPIO_IO(s->gpio)) & 2)) {
			say("camera: gpio%u (%s) is no longer high\n", s->gpio, which == 0 ? "VANA" : "VDIG");
			give_up("a supply's pin somebody else took");
			return false;
		}
	}
	return true;
}

/* ---- the camera subsystem's surroundings, read only: rails, the supplies, the SMMU, camcc's PLLs ---- */

#define SMMU_PHYS               0x15000000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
/*
 * The IFEs' streams, as the base dt's msm_cam_smmu_ife gives them (and Android's SMMU driver programs them): each its own
 * match register, all into the one context. (One register for all four, 0x800 with the mask 0x41f, is not taken: the
 * hypervisor leaves such a write at 0 -- r181. The Wi-Fi's 0x1c00/1 is the dt's own, and is taken.)
 */
static const struct { uint16_t stream, mask; } ife_streams[] = { { 0x808, 0x0 }, { 0x810, 0x8 }, { 0xc08, 0x0 }, { 0xc10, 0x8 } };
#define IFE_STREAMS             (sizeof(ife_streams) / sizeof(ife_streams[0]))

static vm_offset_t smmu;

static void
show_smmu(void)
{
	uint32_t scr0 = rd(smmu + SMMU_SCR0), idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), free_matches = 0;
	bool used[256] = { false };
	unsigned contexts = idr1 & 0xff, free_contexts = 0;
	static const uint32_t ife_sids[] = { 0x808, 0x810, 0x818, 0xc08, 0xc10, 0xc18 };

	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (!(smr >> 31)) {
			free_matches++;
			continue;
		}
		used[s2cr & 0xff] = true;
		for (unsigned i = 0; i < sizeof(ife_sids) / sizeof(ife_sids[0]); i++) {
			if (((stream ^ ife_sids[i]) & ~mask) == 0) {
				say("camera: SMMU match register %u (stream %#x mask %#x, s2cr %#x) covers the IFEs' stream %#x\n", n, stream,
				    mask, s2cr, ife_sids[i]);
				break;
			}
		}
	}
	for (unsigned c = 0; c < contexts; c++) {
		free_contexts += !used[c];
	}
	say("camera: the SMMU: scr0 %#x (a stream nobody matches %s), idr0 %#x idr1 %#x, %u match registers free, %u of %u contexts "
	    "unused by them\n", scr0, scr0 & (1u << 10) ? "faults" : "passes", idr0, idr1, free_matches, free_contexts, contexts);
}

static void
look_around(void)
{
	uint32_t cx = 0, mx = 0, vs = 0, bob = 0;
	bool ready;

	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, 0x10000);
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
	}
	show_rail("cx.lvl", &cx);
	show_rail("mx.lvl", &mx);
	show_rail("vsa1", &vs);
	show_rail("bobb1", &bob);
	(void)show_regulator("the PM8998's LVS1 (VIO)", PID_LVS1, &ready);
	(void)show_regulator("the PMI8998's BOB (VANA's and VDIG's regulators' feed)", PID_BOB, &ready);
	(void)show_regulator("the PM8998's L1 (the CSIPHYs' supply)", PID_L1, &ready);
	{
		vm_offset_t refgen = ml_io_map(REFGEN_PHYS, 0x1000);

		say("camera: refgen (0xff1000): 0x08 %#x, 0x14 %#x (the display's PHY keeps it on, if it is)\n", rd(refgen + 0x08),
		    rd(refgen + 0x14));
	}
	show_smmu();
	show_plls();
}

/* ---- the camera subsystem (Titan 170): rails, PLL2 and PLL3, clocks ------------------------------------ */

/*
 * cam=3 and on: the path a picture takes, tried first with the CSID's own test pattern generator: CSID-lite
 * (0xacc8000, cam_ife_csid_lite170.h) makes RAW10 frames of a colour bar and sends them down its RDI 0 path to VFE-lite
 * (0xacc4000, cam_vfe_lite170.h, the bus of cam_vfe_bus_ver2.c), whose write master 0 writes each frame to memory
 * through the SMMU, into a context of ours. What it takes (camcc-sdm845.c, the base device tree's clocks of cpas, the
 * csiphys and csid-lite):
 *   RPMh   CX and MX at NOM (256), through DRV 2's second TCS, given back (position 0) at the end (cam=1 looks first
 *          whether DRV 2 holds another vote on them)
 *   GCC    camera_axi
 *   camcc  PLL2 (960, even 480) and PLL3 (384), then the RCGs slow_ahb 80 MHz (PLL2 even / 6), cphy_rx 384 (PLL3
 *          even), csiNphytimer 240 (PLL2 even / 2), ife_lite_csid 384 (PLL3 even), ife_lite 480 (PLL2 even), and the branches
 *          cpas_ahb, camnoc_axi, soc_ahb, csiphyN, csiNphytimer, ife_lite_csid, ife_lite_cphy_rx, ife_lite (N: the
 *          sensor's CSIPHY)
 * Everything is taken back in the reverse order at the end, and on any fault.
 */
#define CX_LEVEL                256                     /* NOM */
#define MX_LEVEL                256
#define CSIPHY_TIMER_HZ         240000000u

/*
 * CX and MX are voted for in DRV 2, where the modem's and the audio DSP's drivers vote for them too while those
 * processors boot, and RPMh keeps one value per DRV: the last written. So all three write what
 * drivers/core/hakios.platform.c's hakios_rpmh_shared() gives -- the highest of the three drivers' wishes, under the
 * sequencer's lock --, and one's giving back does not take the others' away. Counted: the sensor's power-up (MCLK runs
 * on PLL2) and the camera subsystem each hold the rails; the last to let go gives the camera's wish back (0).
 */
#define RPMH_CLIENT_CAMERA      2
extern uint32_t hakios_rpmh_shared(unsigned client, uint32_t address, uint32_t value);  /* drivers/core/hakios.platform.c */
extern void hakios_cpufreq_boost(void);                 /* drivers/power/hakios.cpufreq.c (or the stubs' empty one) */
extern void hakios_gpu_boost(void);                     /* drivers/gpu/hakios.a630.c (or the stubs' empty one) */
static bool rails_voted;
static unsigned rails_users;
static uint32_t rails_address[2], rails_ours[2];

/* The position of the first level at or above `level' in RPMh's list for that rail; -1: not there. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;

	*address = rpmh_resource(name, levels, 16, &count);
	for (unsigned l = 0; *address != 0 && l < count; l++) {
		if (levels[l] >= level) {
			return (int)l;
		}
	}
	return -1;
}

/* the camera's wish (`ours' or 0) recorded, and the highest of every driver's written, in one hold of the lock */
static bool
rails_write(bool ours)
{
	uint32_t values[2];
	bool done;

	if (!leaving) {
		lck_mtx_lock(&hakios_rsc_lock);
	}
	for (unsigned r = 0; r < 2; r++) {
		values[r] = hakios_rpmh_shared(RPMH_CLIENT_CAMERA, rails_address[r], ours ? rails_ours[r] : 0);
	}
	done = rpmh_write_locked(2, rails_address, values);
	if (!leaving) {
		lck_mtx_unlock(&hakios_rsc_lock);
	}
	say("camera: RPMh %s CX (%#x) position %u and MX (%#x) position %u (the camera's %s; the highest of the drivers' wishes)\n",
	    done ? "took" : "DID NOT SAY IT TOOK", rails_address[0], values[0], rails_address[1], values[1], ours ? "vote" : "given back");
	return done;
}

/* the camera's hold on CX and MX at NOM */
static bool
rails_get(void)
{
	int cx, mx;

	if (rails_users++ != 0) {
		return rails_voted;
	}
	cx = rail_position("cx.lvl", CX_LEVEL, &rails_address[0]);
	mx = rail_position("mx.lvl", MX_LEVEL, &rails_address[1]);
	if (cx < 0 || mx < 0) {
		say("camera: RPMh has no position for CX %u or MX %u\n", CX_LEVEL, MX_LEVEL);
		return false;
	}
	rails_ours[0] = (uint32_t)cx;
	rails_ours[1] = (uint32_t)mx;
	rails_voted = true;             /* (before the request: one RPMh finishes after our wait must still be given back) */
	return rails_write(true);
}

/* given back by the last that holds them (`all': whatever the count) */
static void
rails_put(bool all)
{
	if (rails_users == 0 || (--rails_users != 0 && !all)) {
		return;
	}
	rails_users = 0;
	if (rails_voted) {
		rails_voted = false;
		(void)rails_write(false);
	}
}

/* An RCG set (source, divider field 2n - 1, no M/N) and updated; then branches on. */
struct rcg {
	const char *    name;
	uint32_t        cmd, source, divider;
	uint32_t        found_cfg;
	bool            set;
};
static struct rcg rcgs[] = {
	{ "slow_ahb 80 MHz", 0x6054, SRC_PLL2_EVEN, 11, 0, false },     /* 480 / 6 */
	{ "cphy_rx 384 MHz", 0x9060, SRC_PLL3_EVEN, 1, 0, false },      /* 384 / 1 */
	{ "csiNphytimer 240 MHz", 0, SRC_PLL2_EVEN, 3, 0, false },      /* 480 / 2 (the sensor's CSIPHY's: hw->timer_cmd) */
	{ "ife_lite_csid 384 MHz", 0xb024, SRC_PLL3_EVEN, 1, 0, false },
	{ "ife_lite 480 MHz", 0xb004, SRC_PLL2_EVEN, 1, 0, false },   /* 480 / 1 (ftbl_cam_cc_ife_0_clk_src's rates) */
};
struct branch {
	const char *    name;
	bool            gcc;
	uint32_t        cbcr;
	bool            set;
};
static struct branch branches[] = {
	{ "gcc camera_axi", true, GCC_CAMERA_AXI_CBCR, false },
	{ "cpas_ahb", false, 0xb11c, false }, { "camnoc_axi", false, 0xb124, false }, { "soc_ahb", false, 0xb13c, false },
	{ "csiphyN", false, 0, false }, { "csiNphytimer", false, 0, false },
	{ "ife_lite_csid", false, 0xb03c, false }, { "ife_lite_cphy_rx", false, 0xb044, false }, { "ife_lite", false, 0xb01c, false },
};

static bool
clocks_up(void)
{
	if (!pll_up(PLL2) || !pll_up(PLL3)) {
		return false;
	}
	/* (the sensor's CSIPHY: its own branch and timer) */
	rcgs[2].cmd = hw->timer_cmd;
	branches[4].cbcr = hw->phy_cbcr;
	branches[5].cbcr = hw->timer_cbcr;
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t cfg = r->cmd + 4;

		r->found_cfg = rd(camcc + cfg);
		if (!camcc_put(cfg, (r->found_cfg & ~0x371fu) | r->source << 8 | r->divider, 0x371f)) {
			return false;
		}
		r->set = true;
		wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
		if (!wait_bits(r->cmd, CMD_UPDATE, 0, 500)) {
			say("camera: the RCG %s does not take its configuration (cmd %#x)\n", r->name, rd(camcc + r->cmd));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (rd(base + b->cbcr) & 1) {
			continue;               /* (on already: left on at the end too) */
		}
		wr(base + b->cbcr, rd(base + b->cbcr) | 1);
		if (!(rd(base + b->cbcr) & 1)) {
			say("camera: the branch %s does not take its enable (%#x)\n", b->name, rd(base + b->cbcr));
			give_up("a clock that does not read back");
			return false;
		}
		b->set = true;
		for (unsigned waited = 0; rd(base + b->cbcr) & CBCR_OFF; waited += 10) {
			if (waited > 500) {
				say("camera: the branch %s does not come on (%#x)\n", b->name, rd(base + b->cbcr));
				give_up("a clock that does not come on");
				return false;
			}
			spin_us(10);
		}
	}
	say("camera: the camera's clocks on (%u RCGs, %u branches)\n", (unsigned)(sizeof(rcgs) / sizeof(rcgs[0])),
	    (unsigned)(sizeof(branches) / sizeof(branches[0])));
	return true;
}

static void
clocks_down(void)
{
	for (int i = (int)(sizeof(branches) / sizeof(branches[0])) - 1; i >= 0; i--) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (b->set) {
			wr(base + b->cbcr, rd(base + b->cbcr) & ~1u);
			b->set = false;
		}
	}
	for (int i = (int)(sizeof(rcgs) / sizeof(rcgs[0])) - 1; i >= 0; i--) {
		struct rcg *r = &rcgs[i];

		if (r->set) {
			wr(camcc + r->cmd + 4, r->found_cfg);
			wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
			(void)wait_bits(r->cmd, CMD_UPDATE, 0, 500);
			r->set = false;
		}
	}
	say("camera: the camera's clocks off\n");
}

/* the PLLs the clocks took (MCLK's hold on PLL2 is its own), the rails titan_up() holds */
static bool titan_plls, titan_rails;

static bool
titan_clocks_up(void)
{
	titan_plls = true;
	return clocks_up();
}

static void
titan_clocks_down(void)
{
	clocks_down();
	if (titan_plls) {
		pll_down(PLL3, false);
		pll_down(PLL2, false);
		titan_plls = false;
	}
}

/* ---- the frames' memory, and the SMMU ---------------------------------------------------------------- */

#define FRAME_ADDRESSES         0x40000000ull           /* the IFE's addresses for it (in dt's iova pool 0x100000-0xfff00000) */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
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
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.sound.c's */

/* What the loader hands to the host (tools/camraw.py reads it). */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, packed (four pixels in five bytes) */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames;         /* how many were written whole */
	uint32_t        frame_bytes;    /* each, from CAM_HEADER_BYTES on */
	uint32_t        source;         /* 0: the CSID's test pattern, 1: the sensor */
	char            sensor[16];
	uint32_t        last;           /* which of the frames' places holds the last one written */
};

extern uint64_t loader_recording[3];
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;

static vm_offset_t frames, smmu_second, smmu_context, page_tables;
static vm_offset_t keep;                /* memory in one piece, taken at boot (hakios_camera_init()) */
static uint64_t keep_bytes;
static uint64_t frames_bytes;
static uint32_t smmu_match[4] = { ~0u, ~0u, ~0u, ~0u }, smmu_old_smr[4], smmu_old_s2cr[4], smmu_matches, smmu_context_n, smmu_pages_size,
    smmu_pages;
static bool smmu_on;

static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, uint32_t source)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (keep != 0 && keep_bytes >= frames_bytes) {
		frames = keep;                  /* (the piece taken at boot: hakios_camera_init()) */
		bzero((void *)frames, CAM_HEADER_BYTES);
	} else if (kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	h->bayer = 0;                   /* (RGGB: the CSID's test pattern; a sensor's picture: capture_sensor()) */
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, !source ? "CSID TPG" : hw->chip, sizeof(h->sensor));
	/* (what the processor wrote out of its caches; the IFE writes past them) */
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("camera: %llu KB of memory for %u frames at %#llx, handed to the loader for the host\n", frames_bytes >> 10, CAM_FRAMES,
	    loader_recording[0]);
	return true;
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1);

	smmu_pages_size = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu_pages * smmu_pages_size != 0x100000 && 2 * smmu_pages * smmu_pages_size != 0x80000) {
		say("camera: the SMMU's layout is not the one known here\n");
		return false;
	}
	smmu_matches = 0;
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			for (unsigned i = 0; i < IFE_STREAMS; i++) {
				if ((((smr & 0x7fff) ^ ife_streams[i].stream) & ~((smr >> 16 & 0x7fff) | ife_streams[i].mask)) == 0) {
					say("camera: SMMU match register %u (%#x) is somebody's for the IFEs' streams: left alone\n", n, smr);
					return false;
				}
			}
		} else if (smmu_matches < IFE_STREAMS) {
			smmu_match[smmu_matches] = n;
			smmu_old_smr[smmu_matches] = smr;
			smmu_old_s2cr[smmu_matches] = s2cr;
			smmu_matches++;
		}
	}
	for (smmu_context_n = 0; smmu_context_n < (idr1 & 0xff) && used[smmu_context_n]; smmu_context_n++) {
	}
	if (smmu_matches < IFE_STREAMS || smmu_context_n >= (idr1 & 0xff)) {
		say("camera: the SMMU has no match register or context left\n");
		return false;
	}
	smmu_second = ml_io_map(SMMU_PHYS + smmu_pages_size, 0x1000);
	smmu_context = ml_io_map(SMMU_PHYS + (uint64_t)(smmu_pages + smmu_context_n) * smmu_pages_size, 0x1000);
	if (rd(smmu_context + SMMU_CB_SCTLR) & 1) {
		say("camera: SMMU context %u is switched on although no stream uses it: somebody's\n", smmu_context_n);
		return false;
	}
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		give_up("an SMMU register that did not read back");
		return false;
	}
	return true;
}

/* The frames' memory behind FRAME_ADDRESSES for the IFEs' streams, through a free context with tables of ours. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);

	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	if (kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = page_tables; line < page_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	say("camera: SMMU: context %u gets our tables for %llu KB at %#llx; match register %u sends streams %#x/%#x through it\n",
	    smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match[0], 0x808, 0);
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) || !smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) ||
	    !smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    !smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) || !smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) || !smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) || !smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
	wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	for (unsigned waited = 0; rd(smmu_context + SMMU_CB_TLBSTATUS) & 1; waited++) {
		if (waited > 1000) {
			lck_mtx_unlock(&hakios_smmu_lock);
			give_up("the SMMU does not finish forgetting");
			return false;
		}
		spin_us(10);
	}
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	smmu_on = true;
	for (unsigned i = 0; i < IFE_STREAMS; i++) {
		if (!smmu_put(smmu, SMMU_S2CR(smmu_match[i]), smmu_context_n, 0x300ff) ||
		    !smmu_put(smmu, SMMU_SMR(smmu_match[i]), 1u << 31 | (uint32_t)ife_streams[i].mask << 16 | ife_streams[i].stream, ~0u)) {
			lck_mtx_unlock(&hakios_smmu_lock);
			return false;
		}
	}
	say("camera: SMMU match registers %u, %u, %u, %u: the IFEs' streams 0x808, 0x810/8, 0xc08, 0xc10/8 into context %u\n",
	    smmu_match[0], smmu_match[1], smmu_match[2], smmu_match[3], smmu_context_n);
	lck_mtx_unlock(&hakios_smmu_lock);
	return true;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("camera: the SMMU has refused the IFE nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("camera: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n",
	    smmu_context_n, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (!smmu_on && smmu_matches == 0) {
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&hakios_smmu_lock);
	}
	for (unsigned i = 0; i < smmu_matches; i++) {
		wr(smmu + SMMU_SMR(smmu_match[i]), smmu_old_smr[i]);
		wr(smmu + SMMU_S2CR(smmu_match[i]), smmu_old_s2cr[i]);
		smmu_match[i] = ~0u;
	}
	if (smmu_on) {
		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	if (!leaving) {
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	smmu_on = false;
	smmu_matches = 0;
	say("camera: the SMMU's match register and context put back as they were found\n");
}

/* ---- CSID-lite 0 and IFE-lite 0 ----------------------------------------------------------------------- */

#define CSID_PHYS               0x0acc8000UL
#define IFE_PHYS                0x0acc4000UL
#define CSIPHY_PHYS(n)          (0x0ac65000UL + 0x1000UL * (n))

#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_CSI2_RX_ECC        0x164
#define CSID_CSI2_RX_CRC        0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b
/*
 * A line's bytes as the write master lays them out: RAW10 (four pixels in five bytes), the line
 * rounded up to 16 bytes, the bus's width (a 4000-pixel line takes 5008, not 5000).
 */
#define RAW10_STRIDE(width)     (((width) * 10 / 8 + 15) & ~15u)

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_MASK1           0x0060
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_CLEAR0          0x2050
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_CLEAR2          0x2058
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_IRQ_GLOBAL_CLEAR    0x2068
#define BUS_COMP_ERROR          0x206c
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_IMAGE_ADDR_OFFSET   0x2218
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static vm_offset_t csid, ife, csiphy;
static bool csid_running, ife_running, tpg_running;

/* A register of the three camera blocks: written, and read back where it is one that keeps what it is given. */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy");
	if (check && rd(base + offset) != value) {
		say("camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		give_up("a camera register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_reg(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

/* cam_ife_csid_global_reset() and reset_regs(): interrupts masked and cleared, then the registers, then the hardware. */
static bool
csid_reset(void)
{
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK + 0x10 * r, 0, false);
	}
	blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (int hw = 1; hw >= 0; hw--) {          /* (the hardware first, then the registers: cam_ife_csid_global_reset() on 170) */
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		if (hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		blk_put(csid, "CSID", CSID_RST_STROBES, hw ? 0x1e : 0x1, false);
		if (!hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!wait_reg(csid, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			say("camera: the CSID's %s reset does not finish (top irq %#x)\n", hw ? "hardware" : "registers'",
			    rd(csid + CSID_TOP_IRQ_STATUS));
			give_up("a CSID that does not reset");
			return false;
		}
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	}
	say("camera: CSID-lite 0 reset (version %#x)\n", rd(csid + CSID_HW_VERSION));
	return true;
}

/* The RX (from a PHY, or the test pattern generator: `phy' -1) and RDI 0, for RAW10 of vc/dt, written as it comes (packed). */
static bool
csid_config(int phy, unsigned lanes, uint32_t lane_cfg, uint32_t vc, uint32_t width, uint32_t height, bool cphy)
{
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0u << 27 | 0xfu << 12 | 0u << 10 | 1u << 2 | 3u;

	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, (lanes - 1) | lane_cfg << 4 | (uint32_t)cphy << 24 |
	    (uint32_t)(phy < 0 ? 0 : phy & 3) << 20,
	    true) || !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (phy < 0) {
		/* cam_ife_csid_config_tpg(): one data type, frames without end, blanking 0x3ff lines and 0x740 pixels, RAW10, colour bars */
		if (!blk_put(csid, "CSID", CSID_TPG_VC_CFG0, 0u << 16 | 1u << 10 | vc, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true)) {
			return false;
		}
	}
	/* cam_ife_csid_init_config_rdi_path() */
	if (!blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	/* cam_ife_csid_enable_rdi_path(): resume at the frame's boundary; the reset, overflow and count errors seen */
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

static bool
tpg_start(void)
{
	/* cam_ife_csid_tpg_start(): four lanes' worth */
	if (!blk_put(csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		return false;
	}
	tpg_running = true;
	return true;
}

/* cam_vfe_top_reset(), cam_vfe_bus_init_hw(), cam_vfe_bus_start_wm() and update_wm() for RDI 0 in frame mode. */
static bool
ife_config(uint64_t address, uint32_t frame_bytes, uint32_t frame_inc)
{
	blk_put(ife, "IFE", IFE_IRQ_MASK0, 1u << 31, false);
	blk_put(ife, "IFE", IFE_GLOBAL_RESET, 0x3f9f, false);
	if (!wait_reg(ife, IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100000)) {
		say("camera: the IFE does not reset (irq %#x)\n", rd(ife + IFE_IRQ_STATUS0));
		give_up("an IFE that does not reset");
		return false;
	}
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	say("camera: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION));
	if (!blk_put(ife, "IFE", BUS_ADDR_SYNC_FRAME_HDR, 0, true) || !blk_put(ife, "IFE", BUS_CGC_OVD, 0xfffff, false) ||
	    !blk_put(ife, "IFE", BUS_TEST_CTRL, 0, false) || !blk_put(ife, "IFE", BUS_ADDR_SYNC_NO_SYNC, 0xffffff, false) ||
	    !blk_put(ife, "IFE", WM0_BURST_LIMIT, 0xf, true) || !blk_put(ife, "IFE", WM0_WIDTH, 0xff01, true) ||
	    !blk_put(ife, "IFE", WM0_HEIGHT, 0, true) || !blk_put(ife, "IFE", WM0_PACKER, 0, true) ||
	    !blk_put(ife, "IFE", WM0_STRIDE, 0xff01, true) || !blk_put(ife, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) || !blk_put(ife, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) || !blk_put(ife, "IFE", BUS_IRQ_MASK1, 1, false) ||
	    !blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)address, false) || !blk_put(ife, "IFE", WM0_FRAME_INC, frame_inc, true) ||
	    !blk_put(ife, "IFE", WM0_CFG, 3, true)) {
		return false;
	}
	ife_running = true;
	/* the RDI 0 resource's start: its register update */
	return blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
}

static void
show_paths(const char *when)
{
	say("camera: %s: CSID rx irq %#x, packets %u, ecc %#x, crc %u; RDI0 irq %#x status %#x, bytes %u/%u; IFE irq %#x %#x, "
	    "violation %#x; bus irq %#x %#x %#x, comp error %#x; WM0 status %#x %#x\n", when, rd(csid + CSID_CSI2_RX_IRQ_STATUS),
	    rd(csid + CSID_CSI2_RX_PKTS), rd(csid + CSID_CSI2_RX_ECC), rd(csid + CSID_CSI2_RX_CRC), rd(csid + CSID_RDI0_IRQ_STATUS),
	    rd(csid + CSID_RDI0_STATUS), rd(csid + CSID_RDI0_BYTES_PING), rd(csid + CSID_RDI0_BYTES_PONG), rd(ife + IFE_IRQ_STATUS0),
	    rd(ife + IFE_IRQ_STATUS1), rd(ife + IFE_VIOLATION_STATUS), rd(ife + BUS_IRQ_STATUS0), rd(ife + BUS_IRQ_STATUS1),
	    rd(ife + BUS_IRQ_STATUS2), rd(ife + BUS_COMP_ERROR), rd(ife + WM0_STATUS0), rd(ife + WM0_STATUS1));
}

static void
paths_down(void)
{
	if (tpg_running) {
		wr(csid + CSID_TPG_CTRL, 0);
		tpg_running = false;
	}
	if (csid_running) {
		/* cam_ife_csid_disable_rdi_path(): halted at once, then the path off */
		wr(csid + CSID_RDI0_IRQ_MASK, 0);
		wr(csid + CSID_RDI0_CTRL, (rd(csid + CSID_RDI0_CTRL) & ~3u) | 2);
		wr(csid + CSID_RDI0_CFG0, rd(csid + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(csid + CSID_CSI2_RX_CFG0, 0);
		csid_running = false;
	}
	if (ife_running) {
		wr(ife + WM0_CFG, 0);
		wr(ife + IFE_REG_UPDATE, 0x2);
		spin_us(1000);
		ife_running = false;
	}
}

/* The camera's blocks up to their clocks; false (and everything taken back by titan_down()) on any fault. */
static bool
titan_up(void)
{
	static vm_offset_t csiphys[4];

	if (csid == 0) {
		csid = ml_io_map(CSID_PHYS, 0x1000);
		ife = ml_io_map(IFE_PHYS, 0x4000);
	}
	if (csiphys[hw->csiphy] == 0) {
		csiphys[hw->csiphy] = ml_io_map(CSIPHY_PHYS(hw->csiphy), 0x1000);
	}
	csiphy = csiphys[hw->csiphy];   /* (the sensor's: another one after a change of sensor) */
	if (!titan_rails) {
		titan_rails = true;
		if (!rails_get()) {
			give_up("RPMh did not take the camera's rails");
			return false;
		}
	}
	if (!(rd(camcc + TITAN_TOP_GDSCR) & GDSC_ON) && !gdsc_on()) {
		return false;
	}
	return titan_clocks_up();
}

static void
titan_down(void)
{
	paths_down();
	smmu_down();
	titan_clocks_down();
	if (titan_rails) {
		titan_rails = false;
		rails_put(false);
	}
}

/* cam=3: a few frames of the CSID's test pattern through RDI 0 and IFE-lite 0 into memory. */
static void
capture_tpg(void)
{
	const uint32_t width = 1600, height = 1200, stride = width * 10 / 8;
	struct cam_header *h;

	if (!frames_alloc(width, height, stride, 0) || !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config(-1, 4, 0x3210, TPG_VC, width, height, false) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height, stride * height)) {
		titan_down();
		return;
	}
	show_paths("configured");
	if (!tpg_start()) {
		titan_down();
		return;
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 2000; ms += 100) {
		wait_ms(100);
		if (rd(ife + BUS_IRQ_STATUS1) & 1) {
			h->frames = 1;
			say("camera: write master 0 says its frame is done after %u ms\n", ms + 100);
			break;
		}
	}
	show_paths("after the frames");
	smmu_faults();
	titan_down();
	/* (what the IFE wrote, a look: the first bytes of the frame, past the processor's caches) */
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=4: the sensor's own frames ------------------------------------------------------------------ */

/*
 * The sensor's tables are Android's, read at run time: tools/camsensor.py makes camera-imx519.tab, camera-imx376k.tab and
 * camera-imx371.tab out of the module files (userland/build.sh puts them into fw/ of the small image). The sequence is
 * Android's: power up, the chip ID, the initialisation ("init"), the mode's own table ("resN": boot-arg cam_mode=N, or
 * preferred_mode()'s), the sensor's CSIPHY, CSID-lite's RX from that PHY and RDI 0, VFE-lite's write master as for
 * cam=3; then "on", the frames, "off", and everything down again. The sensor's writes are not read back (some of its
 * registers start something and clear themselves); each byte must be acknowledged.
 */
/* One of the sensor's tables written in order, as Android's sensor driver writes it. */
static bool
sensor_table(const char *name)
{
	uint32_t count;
	const struct ctab_entry *e = ctab_find(name, &count);

	if (e == NULL) {
		say("camera: the tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t out[4];
		unsigned n = 0;

		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 || e[i].data_bytes > 2) {
			say("camera: \"%s\" %u: an operation not known here (%u, %u/%u bytes)\n", name, i, e[i].operation, e[i].address_bytes,
			    e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		if (e[i].address_bytes == 2) {
			out[n++] = (uint8_t)(e[i].address >> 8);
		}
		out[n++] = (uint8_t)e[i].address;
		if (e[i].data_bytes == 2) {
			out[n++] = (uint8_t)(e[i].data >> 8);
		}
		out[n++] = (uint8_t)e[i].data;
		if (broken || !i2c_transfer(&cci_bus, sensor_address(), out, n, NULL, 0)) {
			say("camera: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
		if (e[i].delay_us != 0) {
			spin_us(e[i].delay_us > 100000 ? 100000 : e[i].delay_us);
		}
	}
	say("camera: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

static bool csiphy_running;

/*
 * The CSIPHY ("qcom,csiphy-v1.0": Android's cam_csiphy_1_0_hwreg.h, cam_csiphy_core.c's cam_csiphy_config_dev()): its
 * reset, the common registers with the lanes to enable, then the two-phase (D-PHY) settings of every lane position or
 * the three-phase (C-PHY) ones of every trio, with the settle count, and its interrupts (status only, nothing handled).
 *
 * The settle count: Android's comes from CamX's settle time; here, for D-PHY, mainline's csiphy_settle_cnt_calc() for
 * the lane's rate and the 240 MHz timer (T_HS_SETTLE = 85 ns + 6 UI, in the timer's periods, less 6); for C-PHY the
 * value Android's table starts with, 0x12. Boot-arg cam_settle=N sets it instead.
 */
static unsigned
csiphy_settle(bool cphy, uint64_t bits_per_lane)
{
	uint32_t forced = 0;
	uint64_t ui_ps, timer_ps = 1000000000000ull / CSIPHY_TIMER_HZ, t_ps;

	if (PE_parse_boot_argn("cam_settle", &forced, sizeof(forced)) && forced != 0 && forced < 0x10000) {
		return forced;
	}
	if (cphy || bits_per_lane == 0) {
		return 0x12;
	}
	/* (mainline: ui = 10^12 / link_freq, then halved: a bit's time, the link frequency being half the bit rate) */
	ui_ps = 1000000000000ull / bits_per_lane;
	t_ps = 85000 + 6 * ui_ps;
	return t_ps / timer_ps > 6 ? (unsigned)(t_ps / timer_ps - 6) : 1;
}

/* the common part: the lanes to enable, then the PHY's reset let go (csiphy_common_reg_1_0) */
static bool
csiphy_common(uint32_t lane_enable)
{
	if (!blk_put(csiphy, "CSIPHY", 0x814, lane_enable, true) || !blk_put(csiphy, "CSIPHY", 0x818, 0x01, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x81c, 0x02, true)) {
		return false;
	}
	wr(csiphy + 0x800, 0x01);
	spin_us(1000);
	wr(csiphy + 0x800, 0x00);
	return true;
}

/* D-PHY: csiphy_2ph_v1_0_reg, every lane position (data lanes 0..3 and the clock lane) */
static bool
csiphy_dphy(unsigned settle)
{
	static const uint32_t lanes[5] = { 0x000, 0x700, 0x200, 0x400, 0x600 };   /* data lane 0, the clock lane, data lanes 1..3 */

	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l];
		bool clock = l == 1;

		if (!blk_put(csiphy, "CSIPHY", b + 0x04, 0x0c, true) || !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x01, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x34, 0x0f, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x0a, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x14, 0x60, true) || (clock && !blk_put(csiphy, "CSIPHY", b + 0x28, 0x04, true)) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x3c, 0xb8, true) || !blk_put(csiphy, "CSIPHY", b + 0x00, clock ? 0x80 : 0x91, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x08, settle & 0xff, true) || (clock && !blk_put(csiphy, "CSIPHY", b + 0x0c, 0xa5, true)) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x10, 0x52, true) || !blk_put(csiphy, "CSIPHY", b + 0x38, clock ? 0x1f : 0xfe, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x60, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x64, 0x7f, true)) {
			return false;
		}
	}
	return true;
}

/* C-PHY: csiphy_3ph_v1_0_reg, every trio position (three) */
static bool
csiphy_cphy(unsigned settle)
{
	static const uint32_t trios[3] = { 0x100, 0x300, 0x500 };

	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t];

		if (!blk_put(csiphy, "CSIPHY", b + 0x5c, 0x63, true) || !blk_put(csiphy, "CSIPHY", b + 0x68, 0xac, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x6c, 0xa5, true) || !blk_put(csiphy, "CSIPHY", b + 0x04, 0x06, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x0c, settle & 0xff, true) || !blk_put(csiphy, "CSIPHY", b + 0x08, (settle >> 8) & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x14, 0x20, true) || !blk_put(csiphy, "CSIPHY", b + 0x50, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x18, 0x3e, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x20, 0x41, true) || !blk_put(csiphy, "CSIPHY", b + 0x24, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x28, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x44, 0x12, true) || !blk_put(csiphy, "CSIPHY", b + 0x60, 0x02, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0xcc, 0x41, true) || !blk_put(csiphy, "CSIPHY", b + 0x64, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0xdc, 0x51, true)) {
			return false;
		}
	}
	return true;
}

/*
 * cam_csiphy_reset(), then the configuration for `lanes' lanes (trios) of the lane assignment, and the interrupt masks.
 * (`settle' is the module file's, said in the log; the count written is csiphy_settle()'s.)
 */
static bool
csiphy_up(unsigned lanes, bool cphy, uint32_t lane_assign, unsigned settle, uint64_t bits_per_lane)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t lane_enable = 0;
	unsigned count = csiphy_settle(cphy, bits_per_lane);

	/* cam_csiphy_config_dev(): D-PHY data lane n at bit 2n, the clock lane at 7; C-PHY trio n at bit 2n + 1 */
	for (unsigned l = 0; l < lanes; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;

		if (n > (cphy ? 2u : 3u)) {
			give_up("a lane assignment not known here");
			return false;
		}
		lane_enable |= cphy ? 1u << (2 * n + 1) : 1u << (2 * n) | 0x80u;
	}
	/* csiphy_reset_reg_1_0 (its delays are milliseconds: cam_csiphy_core.c's usleep_range(delay * 1000)) */
	wr(csiphy + 0x814, 0x00);
	spin_us(5000);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1000);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = true;
	if (!csiphy_common(lane_enable) || !(cphy ? csiphy_cphy(count) : csiphy_dphy(count))) {
		return false;
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + 4 * i, irq_masks[i]);
		wr(csiphy + 0x858 + 4 * i, 0xff);
	}
	wr(csiphy + 0x828, 0x1);
	wr(csiphy + 0x828, 0x0);
	say("camera: CSIPHY %u configured: %u %s at %llu bit/s each (lanes enabled %#x), settle count %u (the module file's %u)\n",
	    hw->csiphy, lanes, cphy ? "trio(s), C-PHY" : "lane(s), D-PHY", bits_per_lane, lane_enable, count, settle);
	return true;
}

static void
show_csiphy(const char *when)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "camera: CSIPHY %u %s, interrupt status:", hw->csiphy, when);

	for (unsigned i = 0; i < 11 && at < (int)sizeof(line) - 6; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", rd(csiphy + 0x8b0 + 4 * i) & 0xff);
	}
	say("%s\n", line);
}

static void
csiphy_down(void)
{
	if (!csiphy_running) {
		return;
	}
	wr(csiphy + 0x814, 0x00);
	spin_us(5000);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1000);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = false;
}

static bool
sensor_write(uint16_t address, uint16_t value, bool sixteen)
{
	uint8_t out[4] = { (uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(value >> 8), (uint8_t)value };

	return sixteen ? i2c_transfer(&cci_bus, sensor_address(), out, 4, NULL, 0) :
	       i2c_transfer(&cci_bus, sensor_address(), (uint8_t[]){ out[0], out[1], out[3] }, 3, NULL, 0);
}

/*
 * The rear cameras' lens motors, powered by a 2.8 V regulator from BOB that a pin switches (GPIO 31, 77; the dtbo's
 * rear_N_actuator_regulator). Their modules come with one of two drivers, and the module files say how each is driven:
 * - an LC898217XC (ON Semiconductor, an 11-bit DAC): 0x72 on CCI master 0 for the main camera, 0x74 on master 1 for the
 *   second (the module files' 0xE4, 0xE8; mainline's sdm845-oneplus); its initialisation a poll of 0xB3 until it reads 0,
 *   the position the 16-bit register 0x84, 0 to 2047 (linux-media's lc898217xc driver): the program's 0-1023 doubled.
 * - an SA3103: 0x0C, 0x0D (the files' 0x18, 0x1A); its initialisation 0x81 = 1, the position's high byte in 0x82 and its
 *   low one in 0x83 (two writes, as the file's two register parameters), 12 bits (the module files' actuator regions
 *   go to 4095, in steps of 384): the program's 0-1023 times four. (r191 took it for 10 bits: the lens went a quarter
 *   of its way at the most, and the search saw no sharpness change.)
 * 0 is no current: the spring's stop, the furthest focus. r190: the developer's main camera did not answer at 0x72.
 */
#define LC_READY                0xb3
#define LC_DAC                  0x84
#define LC_DAC_MAX              2047
#define SA_CONTROL              0x81
#define SA_POSITION_HIGH        0x82
#define SA_POSITION_LOW         0x83
static uint32_t af_position = HAKIOS_CAMERA_NO_FOCUS;
static uint8_t af_chip_address;
static bool af_sa;                      /* the SA3103's way, not the LC898217XC's */

static bool
af_dac(uint32_t position)
{
	if (af_sa) {
		uint32_t code = (position > 1023 ? 1023 : position) * 4;

		return i2c_transfer(&cci_bus, af_chip_address, (uint8_t[]){ SA_POSITION_HIGH, (uint8_t)(code >> 8) }, 2, NULL, 0) &&
		       i2c_transfer(&cci_bus, af_chip_address, (uint8_t[]){ SA_POSITION_LOW, (uint8_t)code }, 2, NULL, 0);
	}
	uint32_t dac = position * 2 > LC_DAC_MAX ? LC_DAC_MAX : position * 2;

	return i2c_transfer(&cci_bus, af_chip_address, (uint8_t[]){ LC_DAC, (uint8_t)(dac >> 8), (uint8_t)dac }, 3, NULL, 0);
}

/* the LC898217XC: answering, and ready (0xB3 reads 0) within 50 ms */
static bool
af_lc_ready(void)
{
	uint8_t reg = LC_READY, status = 0xff;

	for (int i = 0; i < 50; i++) {
		if (i2c_transfer(&cci_bus, hw->af_address, &reg, 1, &status, 1) && status == 0) {
			return true;
		}
		wait_ms(1);
	}
	return false;
}

static void
af_up(bool sensor_frames)
{
	af_position = HAKIOS_CAMERA_NO_FOCUS;
	if (!sensor_frames || hw->af_address == 0 || hw->vaf.kind == SUPPLY_NONE) {
		return;
	}
	if (!supply_on(&hw->vaf, 3)) {
		say("camera: the lens motor's supply did not come on: no focus in this session\n");
		return;
	}
	wait_ms(10);                    /* (8-10 ms from its supply on to its first command: linux-media's driver) */
	if (af_lc_ready()) {
		af_sa = false;
		af_chip_address = hw->af_address;
	} else if (hw->af_sa_address != 0 && i2c_transfer(&cci_bus, hw->af_sa_address, (uint8_t[]){ SA_CONTROL, 0x01 }, 2, NULL, 0)) {
		af_sa = true;
		af_chip_address = hw->af_sa_address;
		wait_ms(1);
	} else {
		say("camera: the lens motor answers neither as an LC898217XC (%#x) nor as an SA3103 (%#x): no focus in this session\n",
		    hw->af_address, hw->af_sa_address);
		supply_off(&hw->vaf, 3);
		return;
	}
	/* from the stop to a quarter of the travel in small steps (a jump knocks the lens: a click to hear) */
	af_position = 0;
	for (uint32_t p = 16; p <= 256; p += 16) {
		if (!af_dac(p)) {
			say("camera: the lens motor stopped answering: no focus in this session\n");
			af_position = HAKIOS_CAMERA_NO_FOCUS;
			supply_off(&hw->vaf, 3);
			return;
		}
		af_position = p;
		wait_ms(2);
	}
	say("camera: the lens motor (%s at %#x) is up, the lens at %u\n", af_sa ? "SA3103" : "LC898217XC", af_chip_address,
	    af_position);
}

/*
 * boot-arg cam_eeprom=1: the main camera module's EEPROM (a CAT24C64 at 0x50 on CCI master 0, the module file's 0xA0:
 * 8 KB, 16-bit addresses), read once, the first time the sensor is up, into the log -- reading only. Its layout is
 * OnePlus's (the module file says to read all of it, not what is where): the OIS's calibration that CamX writes into the
 * BU24218 (39 bytes, OISAddressArray 0x1DC0-0x1DE5 and 0xF006) is somewhere in it, and the lens's infinity and macro.
 */
static bool eeprom_read_once;

static void
eeprom_dump(void)
{
	uint32_t wanted = 0;
	char line[2 * 32 + 1];

	if (eeprom_read_once || hw != &sensors[1] || !PE_parse_boot_argn("cam_eeprom", &wanted, sizeof(wanted)) || wanted == 0) {
		return;
	}
	eeprom_read_once = true;
	say("camera: eeprom: 8192 bytes at 0x50 on CCI master 0 (only lines not all 0xff said)\n");
	for (uint32_t at = 0; at < 8192; at += 32) {
		uint8_t address[2] = { (uint8_t)(at >> 8), (uint8_t)at }, data[32];

		if (!i2c_transfer(&cci_bus, 0x50, address, 2, data, sizeof(data))) {
			say("camera: eeprom: no answer at %#x: the rest not read\n", at);
			return;
		}
		for (int i = 0; i < 32; i++) {
			static const char hex[] = "0123456789abcdef";

			line[2 * i] = hex[data[i] >> 4];
			line[2 * i + 1] = hex[data[i] & 15];
		}
		line[64] = 0;
		bool blank = true;

		for (int i = 0; i < 32; i++) {
			blank = blank && data[i] == 0xff;
		}
		if (!blank) {                   /* (the erased parts left out: the log is a ring, and the boot fills it) */
			say("camera: eeprom %04x: %s\n", at, line);
		}
	}
	say("camera: eeprom: done\n");
}

/*
 * The main camera's OIS, a ROHM BU24218GWL on CCI master 0 (boot-arg cam_ois=1: the servo on, the lens held in the
 * middle of its travel instead of loose on its springs; cam_ois=2, the default: stabilised as for a still; 0: not
 * touched). As CamX drives it from the module file (tools/camsensor.py's ois* tables): powered by the PM8998's L21 at
 * 2.85 V through RPMh (the dtbo's ois_rear_0: cam_vaf-supply, 500 mA) and GPIO 40 high (CAM_OIS_PWD_0); its program
 * written into its RAM (oisinit: 0xF010 = 0, then 2580 bytes from 0x0000); its calibration from the module's EEPROM
 * (oiscal: 39 bytes from 0x0112, each to its register, 0x1DC0-0x1DE5 and 0xF006: the EEPROM block's offset and count are
 * in the module file's EEPROMDriverData); then centring (oiscent) and, for 2, the still mode (oisstill). Nothing here is
 * kept by the chip: its RAM goes with its power. Anything not acknowledged: the OIS down again, the camera goes on.
 */
#define PID_L21                 0x154   /* the PM8998's L21 at 0x5400 (slave 1) */
#define L21_MV                  2856    /* (2.85 V in the dtbo; the LDO's steps are 8 mV) */
#define LDO_MODE_HPM            7       /* PMIC4 LDO: 4 retention, 5 LPM, 7 HPM (rpmh-regulator.c) */
#define OIS_PWD_GPIO            40
static bool ois_on, ois_l21_voted, ois_pwd_switched;
static uint32_t ois_found_cfg, ois_found_io;
static uint8_t ois_address;

static bool
ldo_vote(const char *name, uint32_t mv, bool on, bool *voted)
{
	uint16_t levels[1];
	unsigned count, n = 0;
	uint32_t base = rpmh_resource(name, levels, 1, &count), addresses[3], values[3];
	bool done;

	if (base == 0) {
		say("camera: RPMh has no %s\n", name);
		return false;
	}
	if (on) {
		addresses[n] = base + VRM_VOLTAGE, values[n++] = mv;
		addresses[n] = base + VRM_MODE, values[n++] = LDO_MODE_HPM;
	}
	addresses[n] = base + VRM_ENABLE, values[n++] = on;
	*voted = on;
	done = rpmh_write(n, addresses, values);
	say("camera: RPMh %s %s (%#x) %s\n", done ? "took" : "DID NOT SAY IT TOOK", name, base, on ? "on" : "off (given back)");
	return done;
}

/* an OIS table (address 16 bits, data 8, as the module file's), false at the first write not acknowledged */
static bool
ois_table(const char *name, bool quiet)
{
	uint32_t count = 0;
	const struct ctab_entry *e = ctab_find(name, &count);
	uint64_t t0 = mach_absolute_time(), ns;

	if (e == NULL) {
		say("camera: OIS: the tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t out[3] = { (uint8_t)(e[i].address >> 8), (uint8_t)e[i].address, (uint8_t)e[i].data };

		if (e[i].operation != 0 || e[i].address_bytes != 2 || e[i].data_bytes != 1 ||
		    !i2c_transfer(&cci_bus, ois_address, out, 3, NULL, 0)) {
			say("camera: OIS: \"%s\" %u: %#x <- %#x not done\n", name, i, e[i].address, e[i].data);
			return false;
		}
		if (e[i].delay_us >= 1000) {
			wait_ms((e[i].delay_us + 999) / 1000);
		} else if (e[i].delay_us != 0) {
			spin_us(e[i].delay_us);
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (!quiet) {
		say("camera: OIS: \"%s\": %u writes in %llu ms\n", name, count, ns / 1000000);
	}
	return true;
}

/*
 * The OIS's program the fast way: runs of consecutive addresses written 32 bytes at a time (its RAM's address going up
 * by itself), each run read back and compared; anything not as written, or not read: false, and the caller writes the
 * whole table a byte at a time as the module file has it (2581 writes, 1.2 s at every start of the main camera, r201).
 */
#define OIS_RUN 32
static bool
ois_table_runs(const char *name)
{
	uint32_t count = 0, writes = 0;
	const struct ctab_entry *e = ctab_find(name, &count);
	uint64_t t0 = mach_absolute_time(), ns;

	if (e == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < count;) {
		uint32_t n = 1;
		uint8_t out[2 + OIS_RUN], back[OIS_RUN];

		if (e[i].operation != 0 || e[i].address_bytes != 2 || e[i].data_bytes != 1) {
			return false;
		}
		while (n < OIS_RUN && i + n < count && e[i + n - 1].delay_us == 0 && e[i + n].address == e[i].address + n &&
		    e[i + n].operation == 0 && e[i + n].address_bytes == 2 && e[i + n].data_bytes == 1) {
			n++;
		}
		out[0] = (uint8_t)(e[i].address >> 8);
		out[1] = (uint8_t)e[i].address;
		for (uint32_t k = 0; k < n; k++) {
			out[2 + k] = (uint8_t)e[i + k].data;
		}
		if (!i2c_transfer(&cci_bus, ois_address, out, 2 + n, NULL, 0)) {
			say("camera: OIS: a run of %u at %#x not acknowledged: a byte at a time instead\n", n, e[i].address);
			return false;
		}
		writes++;
		if (n > 1) {
			if (!i2c_transfer(&cci_bus, ois_address, out, 2, back, n) || memcmp(back, out + 2, n) != 0) {
				say("camera: OIS: a run of %u at %#x reads back otherwise (%02x %02x .. not %02x %02x ..): a byte at a time "
				    "instead\n", n, e[i].address, back[0], back[1], out[2], out[3]);
				return false;
			}
		}
		if (e[i + n - 1].delay_us >= 1000) {
			wait_ms((e[i + n - 1].delay_us + 999) / 1000);
		} else if (e[i + n - 1].delay_us != 0) {
			spin_us(e[i + n - 1].delay_us);
		}
		i += n;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	say("camera: OIS: \"%s\": %u entries in %u writes of up to %u, each read back, in %llu ms\n", name, count, writes, OIS_RUN,
	    ns / 1000000);
	return true;
}

static void
ois_down(void)
{
	if (ois_on) {
		/* the lens brought to the middle and held a moment before the servo lets go: less of a knock (r202: a sound at every switch) */
		(void)ois_table("oiscent", true);
		wait_ms(20);
		(void)ois_table("oisstby", true);
		ois_on = false;
	}
	if (ois_pwd_switched) {
		wr(GPIO_IO(OIS_PWD_GPIO), ois_found_io & 2);
		wr(GPIO_CFG(OIS_PWD_GPIO), ois_found_cfg);
		ois_pwd_switched = false;
		say("camera: OIS: gpio%u back as it was found\n", OIS_PWD_GPIO);
	}
	if (ois_l21_voted) {
		(void)ldo_vote("ldoa21", L21_MV, false, &ois_l21_voted);
	}
}

static void
ois_up(void)
{
	uint32_t wanted = 0, count = 0;
	const struct ctab_entry *slave, *cal;
	int l21;

	/* (on unless cam_ois=0: r202, the developer's 6T held in the hand, steadier and sharper) */
	wanted = 2;
	(void)PE_parse_boot_argn("cam_ois", &wanted, sizeof(wanted));
	if (hw != &sensors[1] || wanted == 0) {
		return;
	}
	if ((slave = ctab_find("oisslave", &count)) == NULL || count != 1 || (cal = ctab_find("oiscal", &count)) == NULL) {
		say("camera: OIS: the tables have none (tools/camsensor.py makes them of the module file's OIS)\n");
		return;
	}
	ois_address = (uint8_t)(slave->address >> 1);
	l21 = regulator_state(PID_L21);
	if (l21 < 0) {
		say("camera: OIS: L21 cannot be read: it is not voted for blind\n");
		return;
	}
	if (l21 == 0 && !ldo_vote("ldoa21", L21_MV, true, &ois_l21_voted)) {
		ois_down();
		return;
	}
	spin_us(500);
	if (regulator_state(PID_L21) <= 0) {
		say("camera: OIS: L21 does not come up\n");
		ois_down();
		return;
	}
	ois_found_cfg = rd(GPIO_CFG(OIS_PWD_GPIO));
	ois_found_io = rd(GPIO_IO(OIS_PWD_GPIO));
	ois_pwd_switched = true;
	wr(GPIO_IO(OIS_PWD_GPIO), 2);
	wr(GPIO_CFG(OIS_PWD_GPIO), CFG_OUTPUT | CFG_DRIVE_MA(2));
	say("camera: OIS: L21 %s, gpio%u high: the BU24218 at %#x\n", l21 > 0 ? "on already" : "on", OIS_PWD_GPIO, ois_address);
	wait_ms(10);
	ois_on = true;
	if (!ois_table_runs("oisinit") && !ois_table("oisinit", false)) {
		say("camera: OIS: its program not taken: the OIS down again\n");
		ois_down();
		return;
	}
	/* its calibration, a byte at a time from the EEPROM (the entries' data: the EEPROM's offset) */
	for (uint32_t i = 0; i < count; i++) {
		uint8_t at[2] = { (uint8_t)(cal[i].data >> 8), (uint8_t)cal[i].data }, byte = 0;

		if (!i2c_transfer(&cci_bus, 0x50, at, 2, &byte, 1) ||
		    !i2c_transfer(&cci_bus, ois_address, (uint8_t[]){ (uint8_t)(cal[i].address >> 8), (uint8_t)cal[i].address, byte }, 3,
		    NULL, 0)) {
			say("camera: OIS: calibration byte %u (EEPROM %#x -> %#x) not done: the OIS down again\n", i, cal[i].data,
			    cal[i].address);
			ois_down();
			return;
		}
	}
	say("camera: OIS: its calibration, %u bytes from the EEPROM at %#x\n", count, cal[0].data);
	if (!ois_table("oiscent", false) || (wanted >= 2 && !ois_table("oisstill", false))) {
		say("camera: OIS: not started: the OIS down again\n");
		ois_down();
		return;
	}
	say("camera: OIS: %s\n", wanted >= 2 ? "stabilising (the still mode)" : "the lens held in the middle (centring)");
}

static void
af_move(uint32_t position)
{
	if (af_position == HAKIOS_CAMERA_NO_FOCUS) {
		return;
	}
	position = position > 1023 ? 1023 : position;
	if (position != af_position && af_dac(position)) {
		af_position = position;
	}
}

static void
af_down(void)
{
	if (af_position != HAKIOS_CAMERA_NO_FOCUS) {
		/* back to the stop in small steps and held there a moment, not to knock the lens against it as the current goes */
		while (af_position > 0) {
			uint32_t to = af_position > 16 ? af_position - 16 : 0;

			if (!af_dac(to)) {
				break;
			}
			af_position = to;
			wait_ms(2);
		}
		wait_ms(20);
		af_position = HAKIOS_CAMERA_NO_FOCUS;
	}
	supply_off(&hw->vaf, 3);
}

/* Sony's analog gain register for a linear gain (SONY_GAIN_UNITY is 1x): 1024 - 1024 / gain, at most 16x */
static uint16_t
sony_gain_code(uint32_t gain)
{
	uint32_t code;

	gain = gain < SONY_GAIN_UNITY ? SONY_GAIN_UNITY : gain;
	code = 1024 - (uint32_t)(1024ull * SONY_GAIN_UNITY / gain);
	return (uint16_t)(code > SONY_GAIN_CODE_MAX ? SONY_GAIN_CODE_MAX : code);
}

/*
 * The exposure for cam=4: as the sensor's tables leave it, unless boot-args say otherwise (cam_shutter=LINES,
 * cam_gain=N with 256 as 1x, cam_vblank=LINES: the frame that much longer), written in the sensor's group hold
 * (0x0104, its "holdon"/"holdoff" tables): frame length 0x0340, coarse integration 0x0202, analog gain 0x0204.
 */
static bool
exposure(const struct ctab_mode *mode)
{
	uint32_t shutter = 0, gain = 0, vblank = 0;
	bool ok;

	(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	(void)PE_parse_boot_argn("cam_vblank", &vblank, sizeof(vblank));
	if (shutter == 0 && gain == 0 && vblank == 0) {
		return true;
	}
	vblank = vblank > 0x3fff ? 0x3fff : vblank;
	if (shutter > mode->frame_length + vblank - 40) {
		shutter = mode->frame_length + vblank - 40;     /* (within the frame's lines, as the sensors want) */
	}
	ok = sensor_table("holdon") && (vblank == 0 || sensor_write(0x0340, (uint16_t)(mode->frame_length + vblank), true)) &&
	    (shutter == 0 || sensor_write(0x0202, (uint16_t)shutter, true)) && (gain == 0 || sensor_write(0x0204, sony_gain_code(gain), true)) &&
	    sensor_table("holdoff");
	if (!ok) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	say("camera: the exposure set to %u lines, gain %u/256 (code %#x), %u extra lines a frame\n", shutter, gain,
	    gain ? sony_gain_code(gain) : 0, vblank);
	return true;
}

/*
 * The mode a session takes unless one is asked for: the one with the most pixels of those at 4:3 (within 1 %), at most
 * 61 frames a second and 13 megapixels -- the main camera's 2328x1746 (binned, 60 frames a second at the most: the
 * program's exposure makes it 30), the front one's 2304x1728, the second rear one's 2592x1940.
 */
static uint32_t
preferred_mode(void)
{
	uint64_t best = 0, most = 13000000;
	uint32_t mode_n = 0, full = 0;

	/*
	 * cam_full=1: the sensor's whole array (the IMX519's 4656x3496, the IMX371's 4608x3456), at 30 frames a second at
	 * the most -- four times the binned modes' pixels, for photographs as sharp as the sensor is; 20 MB a frame.
	 */
	if (PE_parse_boot_argn("cam_full", &full, sizeof(full)) && full != 0 && !hw->quad) {
		most = 17000000;
	}
	for (uint32_t m = 0; m < ctab->modes; m++) {
		const struct ctab_mode *o = &ctab_modes[m];
		uint64_t pixels = (uint64_t)o->width * o->height;
		uint32_t a = o->width * 3, b = o->height * 4;

		if ((a > b ? a - b : b - a) * 100 <= b && o->fps_milli <= 61000 && pixels <= most &&
		    (pixels <= 13000000 || o->fps_milli <= 31000) && pixels > best) {
			best = pixels;
			mode_n = m;
		}
	}
	return mode_n;
}

static void
capture_sensor(void)
{
	uint32_t width, height, stride, settle = 0, pattern = 0, wanted = 8, mode_n = ~0u;
	const struct ctab_mode *mode;
	struct cam_header *h;
	char res[8];
	uint64_t rate;

	if (!ctab_load()) {
		return;
	}
	/* the mode: cam_mode=N, or preferred_mode()'s */
	if (!PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n)) || mode_n >= ctab->modes) {
		mode_n = preferred_mode();
	}
	mode = &ctab_modes[mode_n];
	snprintf(res, sizeof(res), "res%u", mode_n);
	width = mode->width;
	height = mode->height;
	stride = RAW10_STRIDE(width);
	rate = (uint64_t)mode->pixel_clock * 10 / (mode->lanes ? mode->lanes : 1);
	(void)PE_parse_boot_argn("cam_frames", &wanted, sizeof(wanted));
	wanted = wanted < 1 ? 1 : wanted > 300 ? 300 : wanted;
	if (!PE_parse_boot_argn("cam_settle", &settle, sizeof(settle)) || settle == 0) {
		settle = mode->settle;  /* (the module file's, for this mode) */
	}
	/* (sides over 16384 would wrap the 32-bit frame size below; a frame_length of 40 or less would wrap exposure()'s `- 40') */
	if (mode->lanes < 1 || mode->lanes > (mode->cphy ? 3u : 4u) || mode->data_type != RAW10_DT || width % 4 != 0 ||
	    width > 16384 || height > 16384 || mode->frame_length <= 40) {
		say("camera: mode %u: %u lanes, data type %#x: not what this is written for\n", mode_n, mode->lanes, mode->data_type);
		return;
	}
	say("camera: mode %u: %ux%u, %u %s at %llu bit/s each, settle count %u\n", mode_n, width, height, mode->lanes,
	    mode->cphy ? "trio(s)" : "lane(s)", rate, settle);
	if (!frames_alloc(width, height, stride, 1)) {
		return;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table(res) || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config((int)hw->csiphy, mode->lanes, ctab->lane_assign & ((1u << (4 * mode->lanes)) - 1), 0, width, height,
	    mode->cphy != 0) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height, stride * height) ||
	    !csiphy_up(mode->lanes, mode->cphy != 0, ctab->lane_assign, settle, rate)) {
		csiphy_down();
		titan_down();
		return;
	}
	show_paths("configured");
	h = (struct cam_header *)frames;
	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	/*
	 * The picture's order is the one each sensor's module file gives its modes (resolutionData's
	 * colorFilterArrangement, in CamX's order: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB -- camera.qcom.so's
	 * TranslateColorFilterArrangement turns it into Android's by the table 3, 2, 1, 0): the OV02B10's
	 * and the OV16A1Q's BGGR, the S5KHM2SD's and the S5K4H7's GRBG (tools/camsensor.py lists it).
	 * Their greens are where a picture of a room has them (the sites of a cell that track each
	 * other); red against blue had been guessed from raw levels under a warm light, the other way
	 * round, and every picture had the two swapped. The OV02B10's own colour bars (cam_pattern=1)
	 * are still taken for GBRG, as before: the module file says nothing of them. A sensor whose
	 * module file has no colour bars (the wide one's and the ultra-wide's have none) gives its
	 * picture instead.
	 */
	uint32_t tpg_count = 0;
	const struct ctab_entry *tpgon = ctab_find("tpgon", &tpg_count);

	if (pattern && (tpgon == NULL || tpg_count == 0 || tpgon[0].address == 0)) {
		say("camera: cam_pattern=1: the %s sensor's module file has no colour bars of its own: its picture\n", hw->name);
		pattern = 0;
	}
	h->bayer = pattern ? hw->pattern_bayer : hw->bayer;
	if ((pattern == 0 || sensor_table("tpgon")) && sensor_table("on") && exposure(mode)) {
		/* frames one after the other into the two places, the last of `wanted' kept (the first ones come before the exposure) */
		for (unsigned ms = 0, done = 0; ms < 5000 && done < wanted; ms += 2) {
			sensor_delay(2000);
			if (!(rd(ife + BUS_IRQ_STATUS1) & 1)) {
				continue;
			}
			done++;
			if (done <= 3 || done == wanted) {
				say("camera: frame %u written, %u ms after the sensor was started\n", done, ms + 2);
			}
			blk_put(ife, "IFE", BUS_IRQ_CLEAR1, 1, false);
			blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
			if (done < wanted) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES + (uint64_t)(done & 1) *
				    stride * height), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
			}
			h->frames = done < CAM_FRAMES ? done : CAM_FRAMES;
			h->last = (done - 1) & 1;
			if ((done & 7) == 0 && !healthy()) {
				break;
			}
		}
		show_paths("after the frames");
		show_csiphy("after the frames");
		(void)sensor_table("off");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=5: the wide camera for Darwin (/dev/hakios-camera) ------------------------------------------- */

/*
 * The sensor's frames for a program (Darwin's camera stack in mediaserverd, libhakios.media.dylib), for
 * as long as it holds /dev/hakios-camera open (sys/hakios.camera.h): the sensor powered up and
 * streaming at the first read after the open, everything down again at the last close, after
 * IDLE_MS without a read, and on any fault (and then for the rest of the run). The sequence is
 * cam=4's. The frames go into three places in turn (kernel memory, mapped page by page through the
 * SMMU): one the IFE writes, one a read may be copying, one ready.
 *
 * Which place the IFE writes: at each start of frame (RDI 0's SOF, polled every millisecond) the
 * address of the place for the frame after it goes to the write master, which takes it at the next
 * SOF (the register update is latched there): a whole frame's time to give it in, not the blanking
 * between two frames. Should no address be waiting at an SOF (the write master's frame increment is
 * 0 here: it then writes the frame where the last one went, not past it), that frame is counted as
 * written over. The exposure the program asks for is written between two frames, after
 * the address, in the sensor's group hold; it counts from the second frame after.
 *
 * A picture for the host (HAKIOS_CAMERA_KEEP: what the program made of a frame) goes into one
 * piece of memory the loader hands over after the run, as cam=3 and cam=4's frames do (xl camera).
 */
#define RING                    3
#define PLACE_SPARE             (256u << 10)    /* room after each frame: some come longer than the mode's lines */
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define EXPOSURE_MARGIN         40
#define GAIN_UNITY              (hw->gain_unity)
#define GAIN_MAX                (hw->gain_max)
#define FRAME_LINES_MAX         0xfff0
#define RDI0_SOF                (1u << 27)      /* IFE_IRQ_STATUS0 (cam_vfe_lite16x.h: sof_irq_mask of RDI 0) */
#define WM0_DONE                1u              /* BUS_IRQ_STATUS1 */

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

static LCK_GRP_DECLARE(darwin_lck_grp, "hakios-camera");
static LCK_MTX_DECLARE(darwin_lock, &darwin_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} place[RING];
static vm_offset_t ring;
static uint64_t ring_bytes, ring_tables_bytes, ring_place_bytes;
static uint32_t ring_frame_bytes, darwin_odd_frames;
static const struct ctab_mode *darwin_mode;
static uint32_t darwin_mode_n, darwin_line_ns;
static volatile bool darwin_open, darwin_streaming, darwin_wanted;  /* open; frames coming; a read wants them */
static uint64_t darwin_last_read, darwin_given, darwin_frames_done, darwin_writes_late;
/* reads copying from the ring now (under darwin_lock): a session's end waits for them before it frees the ring */
static unsigned darwin_readers;
static struct { uint32_t lines, gain, frame_lines; bool pending; uint32_t focus; bool focus_pending; } darwin_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } darwin_now, darwin_next;
/*
 * What the frames are (boot-arg cam_pattern): 0 the sensor's picture; 1 colour bars -- the sensor's own,
 * where its module file has a table for them (Android's "testPatternData"), else the CSID's; 2 the
 * CSID's colour bars (no sensor at all). The wide camera's module file has no colour bars of the
 * sensor's (its one test pattern table is empty), so cam_pattern=1 gives it the CSID's.
 */
enum { SOURCE_SENSOR, SOURCE_SENSOR_PATTERN, SOURCE_CSID_TPG };
static int darwin_source = SOURCE_SENSOR;
/* HAKIOS_CAMERA_SENSOR: the sensor (and mode) to change to, taken between sessions */
static volatile unsigned darwin_switch_to;         /* 1 + the sensor's index; 0: none asked for */
static uint32_t darwin_switch_mode;

static uint64_t
darwin_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* exposure_lines, gain, frame_lines within what the sensor takes (frame_lines 0: the shortest that holds the exposure) */
static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = darwin_mode->frame_length;

	*gain = *gain < GAIN_UNITY ? GAIN_UNITY : *gain > GAIN_MAX ? GAIN_MAX : *gain;
	*lines = *lines < 4 ? 4 : *lines > FRAME_LINES_MAX - EXPOSURE_MARGIN ? FRAME_LINES_MAX - EXPOSURE_MARGIN : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + EXPOSURE_MARGIN > shortest ? *lines + EXPOSURE_MARGIN : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > FRAME_LINES_MAX ? FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - EXPOSURE_MARGIN) {
		*lines = *frame_lines - EXPOSURE_MARGIN;
	}
}

/*
 * The exposure, gain and frame length, written the sensor's way in its group hold (its module file's "holdon" and
 * "holdoff" tables): 0x0340 frame length, 0x0202 coarse integration (lines), 0x0204 analog gain (Sony's code for the
 * program's linear gain).
 */
static bool
exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	bool ok = sensor_table("holdon") && sensor_write(0x0340, (uint16_t)frame_lines, true) && sensor_write(0x0202, (uint16_t)lines, true) &&
	    sensor_write(0x0204, sony_gain_code(gain), true) && sensor_table("holdoff");

	if (!ok) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	return true;
}

static void
ring_free(void)
{
	if (page_tables != 0) {
		kmem_free(kernel_map, page_tables, ring_tables_bytes);
		page_tables = 0;
	}
	if (ring != 0) {
		kmem_free(kernel_map, ring, ring_bytes);
		ring = 0;
	}
	frames = 0;
	frames_bytes = 0;
}

static bool
ring_alloc(uint32_t frame_bytes)
{
	ring_frame_bytes = frame_bytes;
	ring_place_bytes = round_page(frame_bytes) + PLACE_SPARE;
	ring_bytes = RING * ring_place_bytes;
	if (kmem_alloc(kernel_map, &ring, ring_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", RING, ring_bytes >> 10);
		ring = 0;
		return false;
	}
	for (vm_offset_t line = ring; line < ring + ring_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	frames = ring;                  /* (what smmu_up() maps) */
	frames_bytes = ring_bytes;
	ring_tables_bytes = (2 + ((ring_bytes + 0x1fffff) >> 21)) * 4096;
	return true;
}

/*
 * Waits until no read is copying from the ring. Every place is FREE by now, so no new read can start
 * one; this stops ring_free() from freeing the memory under a read that is mid-copy.
 */
static void
darwin_drain_readers(void)
{
	lck_mtx_lock(&darwin_lock);
	while (darwin_readers != 0) {
		(void)msleep(&darwin_readers, &darwin_lock, PZERO, "hakios-camera-drain", NULL);
	}
	lck_mtx_unlock(&darwin_lock);
}

/* The session's end, whatever ended it: the sensor's "off", the blocks, the supplies, the memory. */
static void
darwin_stop(const char *why)
{
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_frames_done);
	if (!broken && darwin_source != SOURCE_CSID_TPG) {
		(void)sensor_table("off");
	}
	af_down();                      /* (the focus lens back to its stop slowly, the OIS still holding it, then the OIS) */
	ois_down();
	/* (only when the paths were set up: a start that failed before them -- a PMIC that did not answer -- has no CSID mapped) */
	if (csid_running || ife_running) {
		show_paths("at the end");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	power_down();
	darwin_drain_readers();
	ring_free();
	say("camera: the frames for Darwin stopped (%s): %llu done, %llu given, %llu written over\n", why, darwin_frames_done, darwin_given,
	    darwin_writes_late);
}

static const char *darwin_res(void);

static bool
darwin_start(void)
{
	uint32_t lines, gain, frame_lines;

	darwin_frames_done = darwin_given = darwin_writes_late = 0;
	darwin_odd_frames = 0;
	if (!ring_alloc(RAW10_STRIDE(darwin_mode->width) * darwin_mode->height)) {
		return false;
	}
	if (darwin_source == SOURCE_CSID_TPG) {
		/* the CSID's colour bars in the mode's size, as cam=3 makes them: no sensor, no CSIPHY */
		if (!titan_up() || !smmu_up() || !csid_reset() ||
		    !csid_config(-1, 4, 0x3210, TPG_VC, darwin_mode->width, darwin_mode->height, false) ||
		    !ife_config(FRAME_ADDRESSES, ring_frame_bytes, 0) || !tpg_start()) {
			return false;
		}
		lck_mtx_lock(&darwin_lock);
		for (int i = 0; i < RING; i++) {
			place[i].state = PLACE_FREE;
		}
		place[0].state = PLACE_WRITING;
		lck_mtx_unlock(&darwin_lock);
		darwin_now.lines = darwin_next.lines = darwin_mode->frame_length - EXPOSURE_MARGIN;
		darwin_now.gain = darwin_next.gain = GAIN_UNITY;
		darwin_now.frame_lines = darwin_next.frame_lines = darwin_mode->frame_length;
		darwin_next.from = 0;
		say("camera: the frames for Darwin: the CSID's colour bars, %ux%u (no sensor)\n", darwin_mode->width, darwin_mode->height);
		return true;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table(darwin_res()) || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config((int)hw->csiphy, darwin_mode->lanes, ctab->lane_assign & ((1u << (4 * darwin_mode->lanes)) - 1), 0, darwin_mode->width,
	    darwin_mode->height, darwin_mode->cphy != 0) || !ife_config(FRAME_ADDRESSES, ring_frame_bytes, 0) ||
	    !csiphy_up(darwin_mode->lanes, darwin_mode->cphy != 0, ctab->lane_assign, darwin_mode->settle,
	    (uint64_t)darwin_mode->pixel_clock * 10 / darwin_mode->lanes)) {
		return false;
	}
	if (darwin_source == SOURCE_SENSOR_PATTERN && !sensor_table("tpgon")) {
		return false;
	}
	lck_mtx_lock(&darwin_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	place[0].state = PLACE_WRITING;
	lines = darwin_ask.lines;
	gain = darwin_ask.gain;
	frame_lines = darwin_ask.frame_lines;
	darwin_ask.pending = false;
	lck_mtx_unlock(&darwin_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	if (!sensor_table("on") || !exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	if (darwin_source == SOURCE_SENSOR) {
		eeprom_dump();
		ois_up();
	}
	af_up(darwin_source == SOURCE_SENSOR);
	darwin_now.lines = darwin_next.lines = lines;
	darwin_now.gain = darwin_next.gain = gain;
	darwin_now.frame_lines = darwin_next.frame_lines = frame_lines;
	darwin_next.from = 0;
	say("camera: the frames for Darwin: mode %u, %ux%u, %u lines of %u ns a frame at the shortest; exposure %u lines, gain %#x, "
	    "frame %u lines\n", darwin_mode_n, darwin_mode->width, darwin_mode->height, darwin_mode->frame_length, darwin_line_ns, lines, gain, frame_lines);
	return true;
}

static void
darwin_session(void)
{
	int writing = 0, next = 0;      /* (place 0's address is the write master's from ife_config(): taken at the first SOF) */
	uint64_t last_frame;

	if (!darwin_start()) {
		give_up("the camera did not start");
		darwin_stop("it did not start");
		return;
	}
	last_frame = darwin_ms();
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = true;
	darwin_last_read = darwin_ms();
	lck_mtx_unlock(&darwin_lock);
	while (!broken && darwin_open && darwin_switch_to == 0) {
		uint32_t top, bus;
		uint64_t now;

		wait_ms(1);
		now = darwin_ms();
		top = rd(ife + IFE_IRQ_STATUS0);
		bus = rd(ife + BUS_IRQ_STATUS1);
		if (!(top & RDI0_SOF) && !(bus & WM0_DONE)) {
			if (now - last_frame > FRAME_TIMEOUT_MS) {
				show_paths("no frame");
				give_up("no frame for 1.5 s");
				break;
			}
			if (now - darwin_last_read > IDLE_MS) {
				break;
			}
			continue;
		}
		blk_put(ife, "IFE", IFE_IRQ_CLEAR0, top & RDI0_SOF, false);
		blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
		blk_put(ife, "IFE", BUS_IRQ_CLEAR1, bus & WM0_DONE, false);
		blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
		lck_mtx_lock(&darwin_lock);
		if (bus & WM0_DONE) {
			uint32_t ping = rd(csid + CSID_RDI0_BYTES_PING), pong = rd(csid + CSID_RDI0_BYTES_PONG);
			uint32_t data = darwin_mode->width * 10 / 8 * darwin_mode->height;

			last_frame = now;
			darwin_frames_done++;
			if ((ping != data || (pong != data && darwin_frames_done > 1)) && darwin_odd_frames++ < 8) {
				say("camera: frame %llu: the CSID counted %u and %u bytes (the mode's frame is %u)\n", darwin_frames_done, ping, pong,
				    data);
			}
			if (darwin_frames_done >= darwin_next.from && darwin_next.from != 0) {
				darwin_now = darwin_next;
				darwin_next.from = 0;
			}
			place[writing].state = PLACE_READY;
			place[writing].sequence = darwin_frames_done;
			place[writing].time = mach_absolute_time();
			place[writing].exposure_lines = darwin_now.lines;
			place[writing].gain = darwin_now.gain;
			place[writing].frame_lines = darwin_now.frame_lines;
		}
		if (top & RDI0_SOF) {
			if (next >= 0) {
				writing = next;         /* (the address given at the last SOF, taken at this one) */
				next = -1;
			} else {
				darwin_writes_late++;      /* no address waiting: this frame goes where the last one went */
			}
			place[writing].state = PLACE_WRITING;
			/* the address for the frame after this one, taken at the next SOF: a whole frame's time to give it in */
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;

				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * ring_place_bytes), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
			}
		}
		bool ask = (bus & WM0_DONE) && darwin_ask.pending, focus = (bus & WM0_DONE) && darwin_ask.focus_pending;
		uint32_t lines = darwin_ask.lines, gain = darwin_ask.gain, frame_lines = darwin_ask.frame_lines, position = darwin_ask.focus;

		darwin_ask.pending = darwin_ask.pending && !ask;
		darwin_ask.focus_pending = darwin_ask.focus_pending && !focus;
		lck_mtx_unlock(&darwin_lock);
		if (focus) {
			af_move(position);
		}
		if (bus & WM0_DONE) {
			wakeup(&darwin_frames_done);
		}
		if (ask && darwin_source != SOURCE_CSID_TPG) {
			exposure_limits(&lines, &gain, &frame_lines);
			if (lines != darwin_now.lines || gain != darwin_now.gain || frame_lines != darwin_now.frame_lines) {
				if (!exposure_write(lines, gain, frame_lines)) {
					break;
				}
				darwin_next.lines = lines;
				darwin_next.gain = gain;
				darwin_next.frame_lines = frame_lines;
				darwin_next.from = darwin_frames_done + 2;
			}
		}
		if ((bus & WM0_DONE) && darwin_frames_done % 30 == 0) {
			uint32_t fsr = rd(smmu_context + SMMU_CB_FSR);

			if (!healthy()) {
				break;
			}
			if (fsr & SMMU_FSR_ALL) {
				smmu_faults();
				give_up("the SMMU refused the IFE");
				break;
			}
			if (darwin_frames_done == 30 || darwin_frames_done % 900 == 0) {
				show_paths("streaming");
			}
		}
	}
	darwin_stop(broken ? "a fault" : !darwin_open ? "closed" : darwin_switch_to != 0 ? "another sensor asked for" : "nobody reads");
}

/*
 * cam_fake=1 (with cam=5; for QEMU): /dev/hakios-camera without the phone's camera. The sessions,
 * sensors, modes and frame headers are cam=5's, from the same tables in fw/, but nothing of the phone
 * is touched: the frames are a colour chart made here -- eight bars (white, beige, red, yellow, green,
 * cyan, blue, 18 % grey) over a grey ramp -- in RAW10 in the sensor's own Bayer order, as bright as the
 * exposure asked for makes them, so that everything above the kernel (the ISP of camera_test and of
 * the media library, the capture graph, Camera.app) runs as it does on the phone and has a known
 * picture to show. cam_fake_fps: frames a second (default 5: QEMU's processor is slow).
 */
static bool fake;

/* the bars' linear reflectance, R, G, B in thousandths */
static const uint16_t fake_bars[8][3] = {
	{ 900, 900, 900 }, { 730, 550, 300 }, { 600, 50, 50 }, { 750, 650, 50 },
	{ 80, 450, 100 }, { 50, 500, 600 }, { 50, 100, 550 }, { 180, 180, 180 },
};

/* one line of the chart, RAW10: `ramp' the grey ramp's, else the bars'; `odd' the second line of the cells */
static void
fake_line(uint8_t *out, uint32_t width, bool odd, bool ramp, uint64_t light)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };
	int r = red[hw->bayer & 3], b = blue[hw->bayer & 3];
	uint32_t v[4];

	for (uint32_t x = 0; x < width; x += 4) {
		for (int i = 0; i < 4; i++) {
			uint32_t px = x + (uint32_t)i, site = (odd ? 2u : 0u) + (px & 1), c = site == (uint32_t)r ? 0 : site == (uint32_t)b ? 2 : 1;
			uint64_t refl = ramp ? (uint64_t)px * 1000 / width : fake_bars[px * 8 / width][c];
			/* (2/15 of a 10-bit step per thousandth of reflectance and line at 1x: 18 % grey at AE's 15 % of white near 6000) */
			uint64_t level = 64 + refl * light * 2 / 15000;

			v[i] = level > 1023 ? 1023 : (uint32_t)level;
		}
		uint8_t *g = out + (x / 4) * 5;

		g[0] = (uint8_t)(v[0] >> 2);
		g[1] = (uint8_t)(v[1] >> 2);
		g[2] = (uint8_t)(v[2] >> 2);
		g[3] = (uint8_t)(v[3] >> 2);
		g[4] = (uint8_t)((v[0] & 3) | (v[1] & 3) << 2 | (v[2] & 3) << 4 | (v[3] & 3) << 6);
	}
}

/* place p filled with the chart as an exposure of `lines' at `gain' shows it */
static void
fake_fill(int p, uint32_t lines, uint32_t gain)
{
	uint8_t *at = (uint8_t *)(ring + (uint64_t)p * ring_place_bytes);
	uint32_t width = darwin_mode->width, height = darwin_mode->height, stride = RAW10_STRIDE(width), ramp_from = height * 3 / 4 & ~1u;
	uint64_t light = (uint64_t)lines * gain / GAIN_UNITY;

	fake_line(at, width, false, false, light);
	fake_line(at + stride, width, true, false, light);
	fake_line(at + (uint64_t)ramp_from * stride, width, false, true, light);
	fake_line(at + (uint64_t)(ramp_from + 1) * stride, width, true, true, light);
	for (uint32_t y = 2; y < height; y++) {
		if (y != ramp_from && y != ramp_from + 1) {
			memcpy(at + (uint64_t)y * stride, at + (uint64_t)((y & 1) + (y >= ramp_from ? ramp_from : 0)) * stride, stride);
		}
	}
}

/* a session of cam_fake: darwin_session()'s, with the chart for the sensor and a timer for its frames */
static void
fake_session(void)
{
	uint32_t fps = 5, lines, gain, frame_lines, filled_lines[RING] = { 0 }, filled_gain[RING] = { 0 };

	(void)PE_parse_boot_argn("cam_fake_fps", &fps, sizeof(fps));
	fps = fps < 1 ? 1 : fps > 30 ? 30 : fps;
	darwin_frames_done = darwin_given = darwin_writes_late = 0;
	if (!ring_alloc(RAW10_STRIDE(darwin_mode->width) * darwin_mode->height)) {
		return;
	}
	lck_mtx_lock(&darwin_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	lines = darwin_ask.lines;
	gain = darwin_ask.gain;
	frame_lines = darwin_ask.frame_lines;
	darwin_ask.pending = false;
	exposure_limits(&lines, &gain, &frame_lines);
	darwin_now.lines = lines;
	darwin_now.gain = gain;
	darwin_now.frame_lines = frame_lines;
	darwin_streaming = true;
	darwin_last_read = darwin_ms();
	lck_mtx_unlock(&darwin_lock);
	say("camera: the frames for Darwin: cam_fake's colour chart, %ux%u in %s's order, %u a second\n", darwin_mode->width, darwin_mode->height,
	    hw->chip, fps);
	while (darwin_open && darwin_switch_to == 0) {
		int p = -1;

		wait_ms(1000 / fps);
		lck_mtx_lock(&darwin_lock);
		if (darwin_ms() - darwin_last_read > IDLE_MS) {
			lck_mtx_unlock(&darwin_lock);
			break;
		}
		if (darwin_ask.pending) {
			lines = darwin_ask.lines;
			gain = darwin_ask.gain;
			frame_lines = darwin_ask.frame_lines;
			darwin_ask.pending = false;
			exposure_limits(&lines, &gain, &frame_lines);
			darwin_now.lines = lines;
			darwin_now.gain = gain;
			darwin_now.frame_lines = frame_lines;
		}
		for (int i = 0; i < RING; i++) {
			if (place[i].state != PLACE_READING && (p < 0 || place[i].sequence < place[p].sequence)) {
				p = i;
			}
		}
		if (p >= 0) {
			place[p].state = PLACE_WRITING;
		}
		lck_mtx_unlock(&darwin_lock);
		if (p < 0) {
			darwin_writes_late++;
			continue;
		}
		if (filled_lines[p] != darwin_now.lines || filled_gain[p] != darwin_now.gain) {
			fake_fill(p, darwin_now.lines, darwin_now.gain);
			filled_lines[p] = darwin_now.lines;
			filled_gain[p] = darwin_now.gain;
		}
		lck_mtx_lock(&darwin_lock);
		place[p].state = PLACE_READY;
		place[p].sequence = ++darwin_frames_done;
		place[p].time = mach_absolute_time();
		place[p].exposure_lines = darwin_now.lines;
		place[p].gain = darwin_now.gain;
		place[p].frame_lines = darwin_now.frame_lines;
		lck_mtx_unlock(&darwin_lock);
		wakeup(&darwin_frames_done);
	}
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_frames_done);
	darwin_drain_readers();
	ring_free();
	say("camera: the frames for Darwin stopped (%s): %llu done, %llu given\n", !darwin_open ? "closed" : darwin_switch_to != 0 ?
	    "another sensor asked for" : "nobody reads", darwin_frames_done, darwin_given);
}

static const char *
darwin_res(void)
{
	static char res[8];

	snprintf(res, sizeof(res), "res%u", darwin_mode_n);
	return res;
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	darwin_open = true;
	return 0;
}

/* (the last close) */
static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	darwin_open = false;
	darwin_wanted = false;
	wakeup(&darwin_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	uint64_t since = darwin_ms();
	int b = -1, error = 0;
	vm_offset_t at = 0;             /* the place's address and the frame's size, taken under the lock with the place */
	uint32_t frame_bytes = 0;

	lck_mtx_lock(&darwin_lock);
	darwin_last_read = darwin_ms();
	if (!darwin_streaming && !darwin_wanted) {
		darwin_wanted = true;
		wakeup(&darwin_open);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&darwin_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > darwin_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (darwin_ms() - since > (darwin_streaming ? 3000u : 15000u)) {      /* (the sensor's tables take seconds) */
			lck_mtx_unlock(&darwin_lock);
			return EAGAIN;
		}
		if (!darwin_streaming && !darwin_wanted) {
			darwin_wanted = true;      /* (the frames stopped meanwhile: another sensor) */
			wakeup(&darwin_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&darwin_frames_done, &darwin_lock, PZERO | PCATCH, "hakios-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&darwin_lock);
			return error;
		}
		darwin_last_read = darwin_ms();
	}
	place[b].state = PLACE_READING;
	/*
	 * Counted as a reader in the same lock hold that chose the place (the ring is live while a place is READY):
	 * a session's end waits for this read, so the ring is not freed under the copy below (a use-after-free).
	 */
	darwin_readers++;
	at = ring + (uint64_t)b * ring_place_bytes;
	frame_bytes = ring_frame_bytes;
	darwin_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = darwin_mode->width;
	f.height = darwin_mode->height;
	f.stride = RAW10_STRIDE(darwin_mode->width);
	f.format = 1;
	f.bayer = darwin_source == SOURCE_CSID_TPG ? 0 : darwin_source == SOURCE_SENSOR_PATTERN ? hw->pattern_bayer : hw->bayer;
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = darwin_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = GAIN_UNITY;
	f.gain_max = GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = darwin_mode->frame_length;
	f.frame_lines_max = FRAME_LINES_MAX;
	f.margin = EXPOSURE_MARGIN;
	f.sensor = hw_index;
	f.focus = af_position;
	lck_mtx_unlock(&darwin_lock);
	/*
	 * (Not the cores at their top speed as well: r187 had them so and the preview was no faster, and r204, the camera
	 * open five minutes with them so, took the hottest core to 97 degrees -- 105 stops the phone. The load drives them.)
	 */
	/*
	 * And the GPU at its fast speed: gpu_dvfs (drivers/gpu/hakios.a630.c) counts submissions, more than six in 100 ms, and
	 * the window server drawing the camera's preview at 20-30 frames a second stays at or under that -- r196's camera
	 * ran with the GPU at 257 MHz the whole time, and the screen showed black patches.
	 */
	hakios_gpu_boost();

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		uint32_t n = (uint32_t)MIN((uint64_t)frame_bytes, (uint64_t)uio_resid(uio));

		uint64_t t0 = mach_absolute_time(), t1, t2;

		/* (what the IFE wrote went past the processor's caches) */
		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		t1 = mach_absolute_time();
		error = uiomove((caddr_t)at, (int)n, uio);
		t2 = mach_absolute_time();
		if (f.sequence <= 3 || f.sequence % 300 == 0) {
			uint64_t a, b;

			absolutetime_to_nanoseconds(t1 - t0, &a);
			absolutetime_to_nanoseconds(t2 - t1, &b);
			say("camera: frame %llu read: %u bytes, the caches' lines forgotten in %llu us, copied in %llu us\n", f.sequence, n,
			    a / 1000, b / 1000);
		}
	}
	lck_mtx_lock(&darwin_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	darwin_last_read = darwin_ms();
	darwin_readers--;                  /* (the copy is over: a session's end waiting to free the ring may go on) */
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_readers);
	return error;
}

/* serialises the writers of `keep' (never held together with darwin_lock) */
static LCK_MTX_DECLARE(keep_lock, &darwin_lck_grp);

/* HAKIOS_CAMERA_KEEP: a picture for the host, in one piece of memory the loader hands over. */
static int
keep_picture(const struct hakios_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(CAM_HEADER_BYTES + (uint64_t)c->bytes);

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (keep == 0 || keep_bytes < want) {
		return ENOSPC;                  /* (the piece is made when the device is, while there is memory in one piece) */
	}
	/*
	 * One writer at a time (its own mutex: darwin_lock is the capture thread's and must not wait on a user copy):
	 * two writers would interleave one picture's header with the other's data in `keep'.
	 */
	lck_mtx_lock(&keep_lock);
	h = (struct cam_header *)keep;
	bzero(h, CAM_HEADER_BYTES);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = c->d == 1 ? hw->bayer : 0;
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "Darwin", sizeof(h->sensor));
	int error = uiomove((caddr_t)(keep + CAM_HEADER_BYTES), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	for (vm_offset_t line = keep; line < keep + want; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(keep);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)keep;
	lck_mtx_unlock(&keep_lock);
	say("camera: a picture from Darwin for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	if (c.command == HAKIOS_CAMERA_EXPOSURE) {
		lck_mtx_lock(&darwin_lock);
		darwin_ask.lines = c.a;
		darwin_ask.gain = c.b;
		darwin_ask.frame_lines = c.c;
		darwin_ask.pending = true;
		lck_mtx_unlock(&darwin_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_KEEP) {
		return keep_picture(&c, uio);
	}
	if (c.command == HAKIOS_CAMERA_FOCUS) {
		lck_mtx_lock(&darwin_lock);
		darwin_ask.focus = c.a;
		darwin_ask.focus_pending = true;
		lck_mtx_unlock(&darwin_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_SENSOR) {
		if (c.a >= sizeof(sensors) / sizeof(sensors[0])) {
			return EINVAL;
		}
		lck_mtx_lock(&darwin_lock);
		darwin_switch_mode = c.b;
		darwin_switch_to = c.a + 1;
		lck_mtx_unlock(&darwin_lock);
		wakeup(&darwin_open);
		return 0;
	}
	return EINVAL;
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The device node, made as the thread starts (before mediaserverd does: its capture stack looks for
 * cameras once, when it starts); reads wait until serve_darwin() is there to answer them.
 */
static bool camdev_made;

static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	camdev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") != NULL;
}

/*
 * The sensor (and its mode) the sessions take: its tables, the mode asked for or else the one with
 * the most pixels of those at 4:3, at most 30 frames a second and 13 megapixels (the wide one's
 * 4000x3000, the front one's 2304x1728, the ultra-wide's 3264x2448, the macro's 1600x1200), the
 * colour bars' source (cam_pattern), the first exposure. False if the sensor cannot be used.
 */
static bool
darwin_select(unsigned index, uint32_t mode_n)
{
	select_sensor(index);
	if (!fake) {
		find_pins();
	}
	if (!ctab_load()) {
		return false;
	}
	if (mode_n >= ctab->modes) {
		mode_n = preferred_mode();
	}
	darwin_mode_n = mode_n;
	darwin_mode = &ctab_modes[mode_n];
	if (darwin_mode->lanes < 1 || darwin_mode->lanes > (darwin_mode->cphy ? 3u : 4u) || darwin_mode->data_type != RAW10_DT ||
	    darwin_mode->width % 4 != 0 || darwin_mode->fps_milli == 0 || darwin_mode->frame_length == 0 ||
	    /* (a frame_length not above the margin makes `frame_length - EXPOSURE_MARGIN' wrap; sides over 16384 make the
	     * 32-bit frame size wrap, so the ring and the IFE's writes would disagree) */
	    darwin_mode->frame_length <= EXPOSURE_MARGIN || darwin_mode->frame_length > FRAME_LINES_MAX || darwin_mode->width > 16384 ||
	    darwin_mode->height > 16384) {
		say("camera: mode %u is not what this is written for\n", mode_n);
		return false;
	}
	darwin_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)darwin_mode->fps_milli * darwin_mode->frame_length));
	uint32_t pattern = 0, count = 0;
	const struct ctab_entry *tpgon;

	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	tpgon = ctab_find("tpgon", &count);
	darwin_source = pattern == 0 ? SOURCE_SENSOR : pattern == 1 && tpgon != NULL && count > 0 && tpgon[0].address != 0 ?
	    SOURCE_SENSOR_PATTERN : SOURCE_CSID_TPG;
	if (pattern != 0) {
		say("camera: cam_pattern=%u: %s\n", pattern, darwin_source == SOURCE_SENSOR_PATTERN ? "the sensor's colour bars (its tpgon table)" :
		    "the CSID's colour bars");
	}
	/* the first exposure, until the program asks for its own: boot-args as cam=4's, else a room's */
	darwin_ask.lines = darwin_mode->frame_length - EXPOSURE_MARGIN;
	darwin_ask.gain = 8 * GAIN_UNITY;
	darwin_ask.frame_lines = 0;
	(void)PE_parse_boot_argn("cam_shutter", &darwin_ask.lines, sizeof(darwin_ask.lines));
	(void)PE_parse_boot_argn("cam_gain", &darwin_ask.gain, sizeof(darwin_ask.gain));
	say("camera: /dev/hakios-camera: the %s camera's frames (mode %u: %ux%u, %u.%03u frames/s at the most), whenever it is "
	    "read\n", hw->name, mode_n, darwin_mode->width, darwin_mode->height, darwin_mode->fps_milli / 1000, darwin_mode->fps_milli % 1000);
	return true;
}

/* cam=5: a session whenever a read wants frames, another sensor whenever one is asked for, until a fault. */
static void
serve_darwin(void)
{
	uint32_t mode_n = ~0u;

	if (!camdev_made) {
		say("camera: /dev/hakios-camera cannot be made\n");
		return;
	}
	(void)PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n));
	if (!darwin_select(hw_index, mode_n)) {
		return;
	}
	while (!broken) {
		lck_mtx_lock(&darwin_lock);
		while (!broken && !(darwin_open && darwin_wanted) && darwin_switch_to == 0) {
			(void)msleep(&darwin_open, &darwin_lock, PZERO, "hakios-camera-idle", NULL);
		}
		unsigned to = darwin_switch_to;

		darwin_switch_to = 0;
		lck_mtx_unlock(&darwin_lock);
		if (broken) {
			break;
		}
		if (to != 0) {
			/* (the last session is over: everything is down) */
			if (!darwin_select(to - 1, darwin_switch_mode)) {
				give_up("a sensor that cannot be used");
				break;
			}
			continue;
		}
		lck_mtx_lock(&darwin_lock);
		darwin_wanted = false;
		lck_mtx_unlock(&darwin_lock);
		if (fake) {
			fake_session();
		} else {
			darwin_session();
		}
	}
	say("camera: /dev/hakios-camera: no more frames in this run\n");
}

/* ---- the look, and the thread ----------------------------------------------------------------------- */

static void
look(void)
{
	bool ready;

	show_clocks();
	show_pin(hw->mclk_pin, "the sensor's MCLK");
	show_pin(hw->sda, "the sensor's CCI data");
	show_pin(hw->scl, "the sensor's CCI clock");
	show_pin(hw->reset, "the sensor's reset");
	show_pin(hw->vana.gpio, "the sensor's VANA switch");
	show_pin(hw->vdig.gpio, "the sensor's VDIG switch");
	(void)show_regulator("LVS1 (VIO)", PID_LVS1, &ready);
	(void)show_regulator("BOB", PID_BOB, &ready);
}

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20;
	char which[16] = "";

	(void)PE_parse_boot_argn("cam_after", &secs, sizeof(secs));
	if (level == 5) {
		camdev_make();
	}
	select_sensor(1);
	if (PE_parse_boot_arg_str("cam_sensor", which, sizeof(which))) {
		for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (strncmp(which, sensors[i].name, strlen(which)) == 0 && (sensors[i].name[strlen(which)] == ' ' ||
			    sensors[i].name[strlen(which)] == '-')) {
				select_sensor(i);
			}
		}
	}
	if (fake) {
		say("camera: cam=5 cam_fake=1: the %s camera's frames for Darwin are a chart made here; nothing of the phone is touched\n",
		    hw->name);
		serve_darwin();
		say("camera: done\n");
		thread_terminate(current_thread());
	}
	wait_ms(secs * 1000);           /* (the system up first: nothing else is on these buses then) */
	tlmm_map();
	gcc = ml_io_map(GCC_PHYS, 0x20000);
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if (rd(arb_core + ARB_VERSION) < 0x50000000 || rd(arb_core + ARB_VERSION) >= 0x70000000) {
		say("camera: the PMIC arbiter's version is %#x, not 5: the supplies cannot be looked at\n", rd(arb_core + ARB_VERSION));
	}
	say("camera: cam=%u: the %s camera%s\n", level, hw->name, level == 5 ? ": its frames for Darwin" :
	    level == 4 ? ": its frames through its CSIPHY and RDI 0 into memory" :
	    level == 3 ? ": the CSID's test pattern through RDI 0 into memory" :
	    level == 2 ? ": powered up, its chip ID read, powered down" : ": a look, reading only");
	find_pins();
	look();
	look_around();
	if (level == 3) {
		capture_tpg();
		power_down();
	} else if (level == 5) {
		serve_darwin();
		power_down();
	} else if (level == 4) {
		capture_sensor();
		power_down();
	} else if (level >= 2) {
		if (ctab_load() && power_up() && healthy()) {
			(void)read_chip_id();
			(void)healthy();
		}
		power_down();
		look();
	}
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
hakios_camera_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 5 ? 5 : level;
	uint32_t fake_n = 0;

	fake = level == 5 && PE_parse_boot_argn("cam_fake", &fake_n, sizeof(fake_n)) && fake_n != 0;
	if (level >= 3) {
		/*
		 * Memory in one piece for the host (cam_keep_mb, default 24: a 12-megapixel NV12 picture of cam=5,
		 * or two frames of cam=3 and cam=4 up to 12 MB each), taken now, while the system's memory still
		 * is in large pieces.
		 */
		uint32_t keep_mb = 24;

		(void)PE_parse_boot_argn("cam_keep_mb", &keep_mb, sizeof(keep_mb));
		keep_bytes = (uint64_t)(keep_mb > 64 ? 64 : keep_mb) << 20;
		if (keep_bytes != 0 && kmem_alloc_contig(kernel_map, &keep, keep_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("camera: no memory in one piece for a picture for the host (%u MB)\n", keep_mb);
			keep = 0;
			keep_bytes = 0;
		}
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: whatever was switched on, off. */
void
hakios_camera_quiesce(void)
{
	if (level < 2 || tlmm == 0) {
		return;
	}
	leaving = true;
	broken = false;                 /* (one last time, whatever went before) */
	csiphy_down();
	titan_down();
	power_down();
}

