/*
 * The touch screen of the OnePlus 6T (fajita, SDM845): a Synaptics controller speaking RMI4 over I2C, on the serial
 * engine at 0xa90000 (QUP wrapper 1, engine 4, the device tree's i2c12), address 0x20. A source of touches for
 * /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg rmi4=1: every address below is this phone's.
 *
 * The sequence is the one the ios-fajita port measured on this phone (its docs/fajita-touch-rmi4-spec.md and patches
 * 0090-0098: the pins of the EAST tile, the two lengths of a GENI command, a register read as two transfers), checked
 * against Linux's i2c-qcom-geni.c, qcom-geni-se.h, gcc-sdm845.c, pinctrl-sdm845.c, rpmh-regulator.c, rmi_f01.c and
 * rmi_f12.c, and the device tree sdm845-oneplus-common.dtsi:
 *   power   VDD 3.0 V from the PM8998's LDO28 ("ldoa28"), voted through RPMh; VIO 1.8 V from a load switch on GPIO88
 *   pins    GPIO49/50 to the serial engine (function 1, qup12); GPIO125 the controller's attention line, an input with
 *           a pull-up; GPIO99 its reset
 *   clocks  the global clock controller's votes for QUP wrapper 1 (core, AHB) and its engine 4; the engine at 19.2 MHz
 *   I2C     the engine ("GENI") in FIFO mode, polled; a register is read as a write of its number, then a read
 *   chip    the page description table; F01 (device control: reset, no sleep, the interrupt status); F12 (2D: ten
 *           objects of eight bytes: state, x, y)
 * No interrupt is taken: F01's interrupt status is read 100 times a second, and F12's objects when it says so.
 *
 * The bring-up is made in steps, each said in the log before it is made; boot-arg rmi4_steps=N stops after the first N:
 *   1 power  2 pins and reset  3 clocks  4 the serial engine  5 the controller (PDT, F01)  6 watch (the default)
 *
 * Boot-args: rmi4=1; rmi4_steps=N; rmi4_khz=100|400 (the bus, 400); rmi4_x=<max>, rmi4_y=<max> (the controller's
 * coordinate range: 1080 x 2340, the panel's own pixels on this phone); soc_rsc=, soc_cmd_db= (RPMh, the SDM845's by default).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
/* RPMh's sequencer (TCS 0 of the RSC): shared with the other drivers that write it */
extern lck_mtx_t hakios_rsc_lock;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
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
	__builtin_arm_dsb(0xf);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ---- power: RPMh ---------------------------------------------------------------------------------- */

#define CMD_DB_SIZE             0x20000UL
#define RSC_SIZE                0x10000UL
static uint64_t rsc_phys = 0x179e0000UL, cmd_db_phys = 0x85fe0000UL;   /* the SDM845's: DRV 2 of the apps RSC, cmd-db */
static vm_offset_t rsc, cmd_db;

#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0               /* millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

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

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		say("rmi4: no command database at %#llx\n", cmd_db_phys);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
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

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	say("rmi4: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			say("rmi4: RPMh: the command slots stay busy\n");
			lck_mtx_unlock(&hakios_rsc_lock);
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/* VDD: the PM8998's LDO28 at 3.008 V, high-power mode (the device tree's vreg_l28a_3p0: 2.856 to 3.008 V) */
static bool
vdd_on(void)
{
	uint32_t base = rpmh_address("ldoa28");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 3008, 1, VRM_MODE_LDO_HPM };

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		say("rmi4: RPMh: no regulator \"ldoa28\" (address %#x)\n", base);
		return false;
	}
	say("rmi4: RPMh: ldoa28 is at %#x; slot status %#x\n", base, rd(TCS(TCS_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	say("rmi4: VDD on (ldoa28, 3.008 V)\n");
	return true;
}

/* ---- pins: TLMM ----------------------------------------------------------------------------------- */

#define TLMM_NORTH_PHYS         0x03900000UL    /* pinctrl-sdm845.c: tile + 0x1000 * pin */
#define TLMM_EAST_PHYS          0x03500000UL
#define GPIO_I2C_SCL            49              /* NORTH */
#define GPIO_I2C_SDA            50              /* NORTH */
#define GPIO_RESET              99              /* NORTH */
#define GPIO_VIO                88              /* EAST */
#define GPIO_ATTN               125             /* EAST */
#define PULL_NONE               0
#define PULL_UP                 3
static vm_offset_t tlmm_north, tlmm_east;

/* ctl: [1:0] pull, [5:2] function, [8:6] drive ((mA / 2) - 1), [9] output enable; io: [0] in, [1] out */
static void
pin(vm_offset_t tile, unsigned n, unsigned function, unsigned pull, unsigned drive, bool output, bool high)
{
	vm_offset_t ctl = tile + 0x1000UL * n, io = ctl + 4;

	if (output) {
		wr(io, high ? 2 : 0);           /* the level first, then the output enable: no glitch */
	}
	wr(ctl, (pull & 3) | (function & 0xf) << 2 | (drive & 7) << 6 | (output ? 1u << 9 : 0));
}

static uint32_t
pin_in(vm_offset_t tile, unsigned n)
{
	return rd(tile + 0x1000UL * n + 4) & 1;
}

/* ---- clocks: GCC ---------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
static vm_offset_t gcc;
#define GCC_VOTE                (gcc + 0x5200c)         /* gcc-sdm845.c: the QUP wrapper 1 votes */
#define VOTE_QUP1_CORE_2X       (1u << 18)
#define VOTE_QUP1_CORE          (1u << 19)
#define VOTE_QUP1_M_AHB         (1u << 20)
#define VOTE_QUP1_S_AHB         (1u << 21)
#define VOTE_QUP1_S4            (1u << 26)
#define GCC_QUP1_S4_CBCR        (gcc + 0x184d4)         /* bit 31: the clock is off */
#define GCC_QUP1_S4_CMD         (gcc + 0x184d8)         /* [0] update, [1] root enable */
#define GCC_QUP1_S4_CFG         (gcc + 0x184dc)         /* [4:0] divider, [10:8] source (0 = XO), [13:12] mode */

static bool
clocks_on(void)
{
	uint32_t cfg;

	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB | VOTE_QUP1_S4);
	sleep_us(50);
	cfg = rd(GCC_QUP1_S4_CFG);
	wr(GCC_QUP1_S4_CFG, (cfg & ~(0x371fu | (1u << 20))) | 1);      /* XO, divided by 1, no M/N */
	wr(GCC_QUP1_S4_CMD, rd(GCC_QUP1_S4_CMD) | 3);
	for (int i = 0; i < 1000 && (rd(GCC_QUP1_S4_CMD) & 1); i++) {
		sleep_us(10);
	}
	for (int i = 0; i < 1000 && (rd(GCC_QUP1_S4_CBCR) >> 31); i++) {
		sleep_us(10);
	}
	say("rmi4: clocks: votes %#x, engine 4 CFG %#x -> %#x, CMD %#x, CBCR %#x (%s)\n", rd(GCC_VOTE), cfg,
	    rd(GCC_QUP1_S4_CFG), rd(GCC_QUP1_S4_CMD), rd(GCC_QUP1_S4_CBCR), (rd(GCC_QUP1_S4_CBCR) >> 31) ? "OFF" : "running");
	return !(rd(GCC_QUP1_S4_CBCR) >> 31);
}

/* ---- I2C: the GENI serial engine ------------------------------------------------------------------- */

#define SE_PHYS                 0x00a90000UL
#define SE_SIZE                 0x4000UL
static vm_offset_t se;
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
#define SE_GENI_TX_FIFO_STATUS  0x800
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
#define TS_ADDRESS              0x20
#define XFER_TIMEOUT_US         20000

static struct {
	uint64_t transfers, failed, nacks;
	uint32_t last_status;
} bus;

static bool
se_init(uint32_t khz)
{
	uint32_t fw = rd(se + GENI_FW_REVISION_RO), depth = (rd(se + SE_HW_PARAM_0) >> 24) & 0x3f;
	uint32_t div, high, low, cycle;

	say("rmi4: serial engine: firmware %#x (protocol %u, 3 is I2C), FIFO %s, depth %u words, status %#x\n", fw,
	    (fw >> 8) & 0xff, (rd(se + GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "on", depth, rd(se + SE_GENI_STATUS));
	if (((fw >> 8) & 0xff) != 3 || (rd(se + GENI_IF_DISABLE_RO) & 1)) {
		say("rmi4: the engine is not an I2C engine with a FIFO: no touch screen\n");
		return false;
	}
	/* i2c-qcom-geni.c's table for a 19.2 MHz engine clock */
	if (khz >= 400) {
		div = 2; high = 5; low = 12; cycle = 24;
	} else {
		div = 7; high = 10; low = 11; cycle = 26;
	}
	wr(se + GENI_CGC_CTRL, rd(se + GENI_CGC_CTRL) | 0x7f);
	wr(se + SE_DMA_GENERAL_CFG, rd(se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(se + GENI_OUTPUT_CTRL, 0x7f);
	wr(se + GENI_FORCE_DEFAULT, 1);
	wr(se + SE_GENI_DMA_MODE_EN, 0);
	wr(se + SE_GSI_EVENT_EN, 0);
	wr(se + SE_IRQ_EN, 0);                                          /* nothing reaches the interrupt controller */
	wr(se + SE_GENI_M_IRQ_EN, M_CMD_DONE | M_TX_FIFO_WATERMARK | M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST | M_ERRORS);
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_GENI_BYTE_GRAN, 0);
	/* four bytes a word, the first byte on the wire in the low byte (the ios-fajita constants, read back) */
	wr(se + SE_GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(se + SE_GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(se + SE_GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(se + SE_GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(se + SE_GENI_TX_WATERMARK, 1);
	wr(se + SE_GENI_RX_WATERMARK, 0);
	wr(se + SE_GENI_RX_RFR_WATERMARK, depth ? depth - 1 : 31);
	wr(se + SE_GENI_CLK_SEL, 0);
	wr(se + GENI_SER_M_CLK_CFG, div << 4 | 1);
	wr(se + SE_I2C_SCL_COUNTERS, high << 20 | low << 10 | cycle);
	say("rmi4: serial engine set up: %u kHz (divider %u, %u/%u/%u), packing %#x %#x\n", khz, div, high, low, cycle,
	    rd(se + SE_GENI_TX_PACKING_CFG0), rd(se + SE_GENI_TX_PACKING_CFG1));
	return true;
}

static void
se_abort(void)
{
	wr(se + SE_GENI_M_CMD_CTRL, 2);                 /* abort */
	for (int i = 0; i < 1000 && !(rd(se + SE_GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
		sleep_us(10);
	}
	while (rd(se + SE_GENI_RX_FIFO_STATUS) & 0x1ffffff) {
		(void)rd(se + SE_GENI_RX_FIFO);
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One I2C transfer to the controller: a write of `len` bytes, or a read of `len` bytes. 0, or an error. */
static int
xfer(bool read, uint8_t *buf, unsigned len)
{
	uint64_t start = mach_absolute_time(), limit;
	uint32_t status = 0;
	unsigned got = 0;

	nanoseconds_to_absolutetime((uint64_t)XFER_TIMEOUT_US * NSEC_PER_USEC, &limit);
	for (int i = 0; rd(se + SE_GENI_STATUS) & 1; i++) {     /* a command still active */
		if (i > 200) {
			se_abort();
			break;
		}
		sleep_us(10);
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_I2C_TX_TRANS_LEN, read ? 0 : len);   /* both lengths for every command */
	wr(se + SE_I2C_RX_TRANS_LEN, read ? len : 0);
	wr(se + SE_GENI_M_CMD0, (read ? I2C_READ : I2C_WRITE) << 27 | TS_ADDRESS << 9);
	if (!read) {
		for (unsigned i = 0; i < len; i += 4) {
			uint32_t word = 0;

			for (unsigned b = 0; b < 4 && i + b < len; b++) {
				word |= (uint32_t)buf[i + b] << (8 * b);
			}
			wr(se + SE_GENI_TX_FIFO, word);
		}
	}
	for (;;) {
		status = rd(se + SE_GENI_M_IRQ_STATUS);
		if (read) {
			uint32_t words = rd(se + SE_GENI_RX_FIFO_STATUS) & 0x1ffffff;

			while (words-- > 0) {
				uint32_t word = rd(se + SE_GENI_RX_FIFO);

				for (unsigned b = 0; b < 4 && got < len; b++) {
					buf[got++] = (uint8_t)(word >> (8 * b));
				}
			}
		}
		if (status & (M_CMD_DONE | M_ERRORS)) {
			break;
		}
		if (mach_absolute_time() - start > limit) {
			status |= 1u << 31;
			break;
		}
	}
	if (read && (status & M_CMD_DONE)) {            /* what came in with the end */
		uint32_t words = rd(se + SE_GENI_RX_FIFO_STATUS) & 0x1ffffff;

		while (words-- > 0) {
			uint32_t word = rd(se + SE_GENI_RX_FIFO);

			for (unsigned b = 0; b < 4 && got < len; b++) {
				buf[got++] = (uint8_t)(word >> (8 * b));
			}
		}
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, status);
	bus.transfers++;
	bus.last_status = status;
	if ((status & (M_ERRORS | 1u << 31)) || !(status & M_CMD_DONE) || (read && got < len)) {
		bus.failed++;
		if (status & M_GP_IRQ_NACK) {
			bus.nacks++;
		}
		if (status & (1u << 31)) {
			se_abort();
		}
		return (status & M_GP_IRQ_NACK) ? 6 /* ENXIO */ : 5 /* EIO */;
	}
	return 0;
}

static int
rmi_read(uint8_t reg, uint8_t *buf, unsigned len)
{
	int error = xfer(false, &reg, 1);       /* the register pointer, then a read: the controller keeps the pointer */

	return error ? error : xfer(true, buf, len);
}

static int
rmi_write(uint8_t reg, uint8_t value)
{
	uint8_t b[2] = { reg, value };

	return xfer(false, b, 2);
}

/* ---- the controller: RMI4 -------------------------------------------------------------------------- */

#define RMI_PAGE_SELECT         0xff
#define RMI_PDT_TOP             0xe9
#define F12_OBJECTS             10
#define F12_OBJECT_SIZE         8
static uint8_t f01_query, f01_cmd, f01_ctrl, f01_data, f12_query, f12_ctrl, f12_data;
static uint8_t f01_irq_mask, f12_irq_mask;
static uint32_t span_x = 1080, span_y = 2340;   /* the controller's coordinates: the panel's pixels */

static bool
rmi_pdt(void)
{
	uint8_t e[6];
	unsigned irq_bit = 0;

	if (rmi_write(RMI_PAGE_SELECT, 0) != 0) {
		say("rmi4: nobody answers at address %#x (the page select write: status %#x)\n", TS_ADDRESS, bus.last_status);
		return false;
	}
	/* entries from 0xe9 down, six bytes each, every one read upwards from its address (Linux's rmi_read_pdt_entry) */
	for (unsigned a = RMI_PDT_TOP; a >= 0x0a; a -= 6) {
		unsigned count;

		if (rmi_read((uint8_t)a, e, 6) != 0) {
			say("rmi4: PDT entry at %#x: read failed (status %#x)\n", a, bus.last_status);
			return false;
		}
		/* an entry, from its lowest address: query, command, control, data base, (version, interrupt count), function */
		if (e[5] == 0 || e[5] == 0xff) {
			break;
		}
		count = e[4] & 7;
		say("rmi4: PDT %#x: function %#04x query %#x command %#x control %#x data %#x, %u interrupt source(s)\n", a, e[5],
		    e[0], e[1], e[2], e[3], count);
		if (e[5] == 0x01) {
			f01_query = e[0]; f01_cmd = e[1]; f01_ctrl = e[2]; f01_data = e[3];
			f01_irq_mask = (uint8_t)(((1u << count) - 1) << irq_bit);
		} else if (e[5] == 0x12) {
			f12_query = e[0]; f12_ctrl = e[2]; f12_data = e[3];
			f12_irq_mask = (uint8_t)(((1u << count) - 1) << irq_bit);
		}
		irq_bit += count;
	}
	if (f01_data == 0 || f12_data == 0) {
		say("rmi4: no F01 or no F12 in the table (F01 data %#x, F12 data %#x)\n", f01_data, f12_data);
		return false;
	}
	say("rmi4: F01 interrupt bits %#x, F12 interrupt bits %#x\n", f01_irq_mask, f12_irq_mask);
	return true;
}

static bool
rmi_f01_init(void)
{
	uint8_t q[21] = { 0 }, status[2] = { 0 };
	char product[11] = { 0 };

	if (rmi_write(f01_cmd, 0x01) == 0) {            /* reset */
		sleep_ms(150);
	}
	(void)rmi_write(RMI_PAGE_SELECT, 0);
	if (rmi_read(f01_query, q, sizeof(q)) == 0) {
		for (unsigned i = 0; i < 10; i++) {
			product[i] = (q[11 + i] >= 0x20 && q[11 + i] < 0x7f) ? (char)q[11 + i] : 0;
		}
		say("rmi4: F01: manufacturer %#x, product \"%s\"\n", q[0], product);
	}
	/* no sleep, "configured" (bit 7: the controller clears it after a reset of its own); every interrupt source on */
	if (rmi_write(f01_ctrl, 0x84) != 0 || rmi_write((uint8_t)(f01_ctrl + 1), 0x7f) != 0) {
		say("rmi4: F01: the control write failed (status %#x)\n", bus.last_status);
		return false;
	}
	if (rmi_read(f01_data, status, sizeof(status)) == 0) {
		say("rmi4: F01: device status %#x, interrupt status %#x\n", status[0], status[1]);
	}
	return true;
}

/* ---- touches --------------------------------------------------------------------------------------- */

static struct {
	bool down;
	uint32_t x, y;
} contact[F12_OBJECTS];
static uint32_t downs;

static void
report(const uint8_t *d)
{
	for (unsigned id = 0; id < F12_OBJECTS; id++) {
		const uint8_t *o = d + id * F12_OBJECT_SIZE;
		bool present = o[0] != 0;

		if (present) {
			uint32_t rx = o[1] | (uint32_t)(o[2] & 0x0f) << 8, ry = o[3] | (uint32_t)(o[4] & 0x0f) << 8;
			uint32_t x = rx >= span_x ? 65535 : rx * 65535 / span_x, y = ry >= span_y ? 65535 : ry * 65535 / span_y;

			if (!contact[id].down) {
				contact[id].down = true;
				downs++;
				if (downs <= 20) {
					say("rmi4: finger %u down at %u, %u\n", id, rx, ry);
				}
				hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
			} else if (x != contact[id].x || y != contact[id].y) {
				hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
			}
			contact[id].x = x;
			contact[id].y = y;
		} else if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

static void
lift_all(void)
{
	for (unsigned id = 0; id < F12_OBJECTS; id++) {
		if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

static void
rmi4_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, khz = 400;
	unsigned errors = 0, said_status = 0, said_peek = 0, said_raw = 0, attn_changes = 0;
	uint32_t last_attn = 2;
	uint8_t last_raw[32];
	uint32_t peek = 1;
	uint64_t ticks = 0;

	PE_parse_boot_argn("rmi4_khz", &khz, sizeof(khz));
	PE_parse_boot_argn("rmi4_peek", &peek, sizeof(peek));
	PE_parse_boot_argn("rmi4_x", &span_x, sizeof(span_x));
	PE_parse_boot_argn("rmi4_y", &span_y, sizeof(span_y));
	(void)PE_parse_boot_argn("soc_rsc", &rsc_phys, sizeof(rsc_phys));
	(void)PE_parse_boot_argn("soc_cmd_db", &cmd_db_phys, sizeof(cmd_db_phys));
	if (span_x == 0 || span_y == 0) {
		span_x = 1080;
		span_y = 2340;
	}

	say("rmi4: step 1: power (RPMh at %#llx, cmd-db at %#llx; GPIO%u)\n", rsc_phys, cmd_db_phys, GPIO_VIO);
	rsc = ml_io_map(rsc_phys, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(cmd_db_phys, CMD_DB_SIZE);
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, 0x64000);
	tlmm_east = ml_io_map(TLMM_EAST_PHYS, 0x7e000);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (!rsc || !cmd_db || !tlmm_north || !tlmm_east || !gcc || !se) {
		say("rmi4: a mapping failed\n");
		goto out;
	}
	if (!vdd_on()) {
		say("rmi4: VDD not voted: going on all the same (the boot loader may have left it on)\n");
	}
	sleep_ms(10);
	pin(tlmm_east, GPIO_VIO, 0, PULL_NONE, 3, true, true);
	sleep_ms(10);
	say("rmi4: VIO on (GPIO%u high)\n", GPIO_VIO);
	if (steps < 2) {
		goto out;
	}

	say("rmi4: step 2: pins: I2C on GPIO%u/%u, attention GPIO%u, reset GPIO%u\n", GPIO_I2C_SCL, GPIO_I2C_SDA, GPIO_ATTN,
	    GPIO_RESET);
	pin(tlmm_north, GPIO_I2C_SCL, 1, PULL_NONE, 0, false, false);
	pin(tlmm_north, GPIO_I2C_SDA, 1, PULL_NONE, 0, false, false);
	pin(tlmm_east, GPIO_ATTN, 0, PULL_UP, 3, false, false);
	pin(tlmm_north, GPIO_RESET, 0, PULL_UP, 2, true, true);
	sleep_ms(10);
	pin(tlmm_north, GPIO_RESET, 0, PULL_UP, 2, true, false);
	sleep_ms(10);
	pin(tlmm_north, GPIO_RESET, 0, PULL_UP, 2, true, true);
	sleep_ms(200);
	say("rmi4: reset done; attention line %s\n", pin_in(tlmm_east, GPIO_ATTN) ? "high" : "LOW (the controller has news)");
	if (steps < 3) {
		goto out;
	}

	say("rmi4: step 3: clocks\n");
	if (!clocks_on()) {
		say("rmi4: the engine's clock does not run: no touch screen\n");
		goto out;
	}
	if (steps < 4) {
		goto out;
	}

	say("rmi4: step 4: the serial engine at %#lx\n", SE_PHYS);
	if (!se_init(khz)) {
		goto out;
	}
	if (steps < 5) {
		goto out;
	}

	say("rmi4: step 5: the controller (address %#x)\n", TS_ADDRESS);
	if (!rmi_pdt() || !rmi_f01_init()) {
		say("rmi4: %llu transfers, %llu failed (%llu not acknowledged): no touch screen\n", bus.transfers, bus.failed, bus.nacks);
		goto out;
	}
	if (steps < 6) {
		goto out;
	}

	{
		uint8_t q[16] = { 0 }, c[16] = { 0 };

		if (rmi_read(f12_query, q, sizeof(q)) == 0 && rmi_read(f12_ctrl, c, sizeof(c)) == 0) {
			say("rmi4: F12 query %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
			say("rmi4: F12 control %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
		}
	}
	memset(last_raw, 0xa5, sizeof(last_raw));
	{
		uint8_t flush[F12_OBJECTS * F12_OBJECT_SIZE];

		(void)rmi_read(f12_data, flush, sizeof(flush));     /* the report waiting since the reset: read it, or no other comes */
	}
	say("rmi4: step 6: watching, every 10 ms (coordinates up to %u x %u)\n", span_x, span_y);
	for (;;) {
		uint8_t status[2], d[F12_OBJECTS * F12_OBJECT_SIZE];

		if (rmi_read(f01_data, status, sizeof(status)) != 0) {
			if (++errors == 20) {
				say("rmi4: F01 reads keep failing (status %#x)\n", bus.last_status);
			}
			if (errors > 500) {
				say("rmi4: giving up on the controller\n");
				lift_all();
				break;
			}
			sleep_ms(10);
			continue;
		}
		errors = 0;
		if (status[0] & 0x80) {
			say("rmi4: the controller reset itself (device status %#x): set up again\n", status[0]);
			(void)rmi_f01_init();
		}
		{
			uint32_t attn = pin_in(tlmm_east, GPIO_ATTN);

			if (attn != last_attn) {
				attn_changes++;
				if (attn_changes <= 20) {
					say("rmi4: attention line now %s (change %u, interrupt status %#x)\n", attn ? "high" : "LOW", attn_changes,
					    status[1]);
				}
				last_attn = attn;
			}
		}
		if (ticks % 50 == 0 && said_raw < 15) {
			uint8_t raw[32];

			if (rmi_read(f12_data, raw, sizeof(raw)) == 0 && memcmp(raw, last_raw, sizeof(raw)) != 0) {
				said_raw++;
				memcpy(last_raw, raw, sizeof(raw));
				say("rmi4: F12 data now %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  "
				    "%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11],
				    raw[12], raw[13], raw[14], raw[15], raw[16], raw[17], raw[18], raw[19], raw[20], raw[21], raw[22],
				    raw[23], raw[24], raw[25], raw[26], raw[27], raw[28], raw[29], raw[30], raw[31]);
			}
		}
		if (status[1] != 0 && said_status < 30) {
			said_status++;
			say("rmi4: interrupt status %#x (device status %#x), attention line %s\n", status[1], status[0],
			    pin_in(tlmm_east, GPIO_ATTN) ? "high" : "low");
		}
		/*
		 * F12's objects when its interrupt bit says so -- and, to find out what this controller does, twice a second
		 * whatever the bit says (rmi4_peek=0 turns that off once the bit is known to work)
		 */
		/*
		 * F12's objects: when its interrupt bit says so, when the attention line is low, and every 100 ms whatever they
		 * say. This controller (S3706B) raises no new report until the last one has been read (11-10: with the objects
		 * read only on the bit, a whole session saw no touch; with a look twice a second, touches came through), so a
		 * report that is never read stops the reports for good.
		 */
		if ((status[1] & f12_irq_mask) || !pin_in(tlmm_east, GPIO_ATTN) || ticks % 10 == 0 || (peek && ticks % 50 == 0)) {
			if (rmi_read(f12_data, d, sizeof(d)) == 0) {
				if (!(status[1] & f12_irq_mask) && said_peek < 10) {
					for (unsigned id = 0; id < F12_OBJECTS; id++) {
						if (d[id * F12_OBJECT_SIZE] != 0) {
							said_peek++;
							say("rmi4: F12 has object %u (%02x %02x %02x %02x %02x %02x) without its interrupt bit\n", id,
							    d[id * 8], d[id * 8 + 1], d[id * 8 + 2], d[id * 8 + 3], d[id * 8 + 4], d[id * 8 + 5]);
							break;
						}
					}
				}
				report(d);
			}
		}
		if (++ticks % 6000 == 0) {
			say("rmi4: %llu transfers, %llu failed; %u touches so far\n", bus.transfers, bus.failed, downs);
		}
		sleep_ms(10);
	}
out:
	if (steps < 6) {
		say("rmi4: stopping here (rmi4_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_rmi4_init(void)
{
	uint32_t enabled = 0, steps = 6;
	thread_t thread;

	if (!PE_parse_boot_argn("rmi4", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("rmi4_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(rmi4_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
