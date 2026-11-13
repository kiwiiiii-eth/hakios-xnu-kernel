/*
 * Bluetooth of the OnePlus 6T (fajita, SDM845): the Bluetooth half of its WCN3990, on a UART. Only with boot-arg bt=N; every
 * address below is this phone's. The Wi-Fi half of the same chip is drivers/wifi/hakios.wifi10.c; the half here does not
 * touch it, nor any supply: the chip's supplies (vddio S4A 1.8 V, vddxo L7A 1.8 V, vddrf L17A 1.3 V, vddch0 L25A 3.3 V,
 * sdm845-oneplus-common.dtsi) are on from the boot loader and the Wi-Fi's start, and nothing here writes the PMIC.
 *
 * Where things are (checked against Linux's sdm845.dtsi, sdm845-oneplus-common.dtsi, gcc-sdm845.c, pinctrl-sdm845.c,
 * qcom_geni_serial.c, geni-se.h, qcom-geni-se.c, hci_qca.c and btqca.c; Linux's are GPL, nothing of them is copied here,
 * only the numbers of the hardware and the order of the chip's protocol):
 *   UART    QUP wrapper 0, serial engine 6 at 0x898000 (the tree's uart6, "qcom,geni-uart"), FIFO mode, polled, 8N1
 *   pins    GPIO45 CTS, GPIO46 RFR, GPIO47 TX, GPIO48 RX: function 1 (qup6), the EAST tile of the TLMM (0x3500000)
 *   clocks  GCC's votes at 0x5200c: bits 6 and 7 (the wrapper's AHB), 16 (engine 6), and 8, 9 (the wrapper's core clocks,
 *           by the place of wrapper 1's 18 and 19 that hakios.rmi4.c votes); engine 6's RCG at 0x17754, set to 7.3728 MHz
 *           (GPLL0's even output, 300 MHz, times 384/15625), its branch at 0x17750
 *   speed   oversampling 32: 115200 baud is that clock divided by 2, 2400 by 96 (both exact)
 *   chip    the WCN3990's own way on (hci_qca.c, qca_regulator_init and qca_send_power_pulse): with the CTS line ignored,
 *           the byte 0xc0 at 2400 baud (it puts the chip into its boot mode), then 0xfc at 115200 (on); an outside circuit at
 *           its RX pin decodes those two bytes, at those speeds. Then Qualcomm's vendor command 0xfc00: 0x19 the version
 *           (answered by a vendor event 0xff: 00 02, product, patch, ROM, SoC), 0x1e a piece of a file of at most 243 bytes
 *           (each answered 00 04 <result>, unless the patch's download mode says only the last is); the rampatch
 *           crbtfwNN.tlv, then the NVM crnvNN.bin, NN from the ROM and SoC versions (21 on this phone); HCI_Reset; the
 *           address with 0xfc14. The files are the phone's own, from its partition "bluetooth" (IMAGE/CRBTFW21.TLV and
 *           IMAGE/CRNV21.BIN), put by BT=1 userland/build.sh into /fw of the system image from ../fw-bt/ (lower case), read once the root is mounted.
 *   NVM     tag 17 (HCI transport): its in-band sleep bit (0x80 of the first byte) cleared and its third byte, the UART
 *           speed, set to this driver's (0, 115200); tag 27 (deep sleep): bit 0 cleared. Linux sets both: it has
 *           Qualcomm's in-band sleep protocol (bytes 0xfc 0xfd 0xfe between packets); this driver has not.
 *
 * Levels (bt=N), each said in the log before it is made:
 *   1  a look, reading only: the clocks' registers, the four pins, and the engine's own registers only if its clocks run
 *   2  the clocks, the pins, the engine as a UART; the chip on, its version, the chip off again (0xc0 at 2400)
 *   3  and the rampatch, the NVM, HCI_Reset, the address, the six commands a host asks first (hakios.bthci.c); then off
 *   4  and the controller given to iOS: the registry's "bluetooth", the nodes, the kernel control, the Skywalk pipe,
 *      and the bridge to bluetoothd (hakios.bthci.c); it stays on
 *
 * Boot-args: bt=N; bt_after=S (the start, this many seconds after boot: 30); bt_mac=aa:bb:cc:dd:ee:ff (the address;
 * else the Wi-Fi's, wifi_mac, plus one in its last byte; else the NVM's)
 *
 * Status: compiled only (11-13). Not run on the phone.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);
extern void hakios_bt_say(const char *format, ...) __printflike(1, 2);                                     /* hakios.bthci.c */
extern bool hakios_bthci_start(const char *name, int (*send)(const uint8_t *, unsigned), int (*receive)(uint8_t *, unsigned, unsigned),
    void (*off)(void), const uint8_t *address, bool publish);

#define say(...) hakios_bt_say("bt3990: " __VA_ARGS__)

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
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static uint32_t
ms_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/* ---- clocks: GCC ------------------------------------------------------------------------------------ */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
static vm_offset_t gcc;
#define GCC_GPLL0_MODE          (gcc + 0x00000)         /* bit 31: locked */
#define GCC_GPLL0_USER_CTL      (gcc + 0x0000c)         /* its outputs (said, not written) */
#define GCC_VOTE                (gcc + 0x5200c)
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_S6            (1u << 16)
#define VOTES                   (VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB | VOTE_QUP0_CORE | VOTE_QUP0_CORE_2X | VOTE_QUP0_S6)
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x17004)
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x17008)
#define GCC_QUP0_S6_CBCR        (gcc + 0x17750)         /* bit 31: the clock is off */
#define GCC_QUP0_S6_CMD         (gcc + 0x17754)         /* [0] update, [1] root enable */
#define GCC_QUP0_S6_CFG         (gcc + 0x17758)         /* [4:0] divider (2n-1), [10:8] source (0 XO, 6 GPLL0 even), [13:12] mode (2: M/N) */
#define GCC_QUP0_S6_M           (gcc + 0x1775c)
#define GCC_QUP0_S6_N           (gcc + 0x17760)         /* ~(N - M) */
#define GCC_QUP0_S6_D           (gcc + 0x17764)         /* ~(2D), 2D = N: half the time high */
#define S6_CLOCK_HZ             7372800u
#define S6_M                    384u
#define S6_N                    15625u

static bool
clocks_running(void)
{
	return !(rd(GCC_QUP0_S6_CBCR) >> 31) && !(rd(GCC_QUP0_M_AHB_CBCR) >> 31) && !(rd(GCC_QUP0_S_AHB_CBCR) >> 31);
}

static void
say_clocks(const char *when)
{
	say("%s: GPLL0 mode %#x user %#x; votes %#x (ours %#x: %s); AHB m %#x s %#x; engine 6 CBCR %#x CMD %#x CFG %#x M %#x N %#x D %#x: %s\n",
	    when, rd(GCC_GPLL0_MODE), rd(GCC_GPLL0_USER_CTL), rd(GCC_VOTE), VOTES, (rd(GCC_VOTE) & VOTES) == VOTES ? "all" : "not all",
	    rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR), rd(GCC_QUP0_S6_CBCR), rd(GCC_QUP0_S6_CMD), rd(GCC_QUP0_S6_CFG),
	    rd(GCC_QUP0_S6_M), rd(GCC_QUP0_S6_N), rd(GCC_QUP0_S6_D), clocks_running() ? "RUNNING" : "off");
}

static bool
clocks_on(void)
{
	uint32_t cfg;

	say("clocks: the votes for QUP wrapper 0 and its engine 6, then engine 6 at %u Hz (GPLL0 even x %u/%u)\n", S6_CLOCK_HZ, S6_M, S6_N);
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTES);
	sleep_us(50);
	wr(GCC_QUP0_S6_M, S6_M);
	wr(GCC_QUP0_S6_N, ~(S6_N - S6_M) & 0xffff);
	wr(GCC_QUP0_S6_D, ~S6_N & 0xffff);
	cfg = rd(GCC_QUP0_S6_CFG);
	wr(GCC_QUP0_S6_CFG, (cfg & ~(0x371fu | (1u << 20))) | 1u /* divided by 1 */ | 6u << 8 /* GPLL0 even */ | 2u << 12 /* M/N */);
	wr(GCC_QUP0_S6_CMD, rd(GCC_QUP0_S6_CMD) | 3);
	for (int i = 0; i < 1000 && (rd(GCC_QUP0_S6_CMD) & 1); i++) {
		sleep_us(10);
	}
	for (int i = 0; i < 1000 && !clocks_running(); i++) {
		sleep_us(10);
	}
	say_clocks("clocks");
	return clocks_running();
}

/* ---- pins: TLMM, the EAST tile ------------------------------------------------------------------------ */
#define TLMM_EAST_PHYS          0x03500000UL
#define TLMM_EAST_SIZE          0x40000UL
static vm_offset_t tlmm_east;
/* ctl: [1:0] pull (0 none, 1 down, 3 up), [5:2] function, [8:6] drive ((mA / 2) - 1), [9] output enable; io: [0] in, [1] out */
static const struct {
	unsigned n, pull;
	const char *what;
} pins[] = {
	{ 45, 1, "CTS (the chip's RTS: may we send)" },
	{ 46, 0, "RFR (our RTS: may it send)" },
	{ 47, 0, "TX" },
	{ 48, 3, "RX" },
};

static void
say_pins(const char *when)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		vm_offset_t ctl = tlmm_east + 0x1000UL * pins[i].n;
		uint32_t c = rd(ctl), io = rd(ctl + 4);

		say("%s: gpio%u %s: ctl %#x (function %u, pull %u, %u mA%s), reads %u\n", when, pins[i].n, pins[i].what, c, (c >> 2) & 15,
		    c & 3, (((c >> 6) & 7) + 1) * 2, (c & (1u << 9)) ? ", an output" : "", io & 1);
	}
}

static void
pins_to_uart(void)
{
	say("pins: gpio45..48 to the serial engine (function 1, qup6), 2 mA\n");
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		wr(tlmm_east + 0x1000UL * pins[i].n, (pins[i].pull & 3) | 1u << 2);
	}
	say_pins("pins");
}

/* ---- the UART: GENI serial engine 6 ------------------------------------------------------------------- */
#define SE_PHYS                 0x00898000UL
#define SE_SIZE                 0x4000UL
static vm_offset_t se;
#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define SE_GENI_STATUS          0x040           /* [0] the main sequencer's command, [12] the secondary's */
#define GENI_SER_M_CLK_CFG      0x048           /* [15:4] divider, [0] on */
#define GENI_SER_S_CLK_CFG      0x04c
#define GENI_IF_DISABLE_RO      0x064
#define GENI_FW_REVISION_RO     0x068           /* [15:8] the protocol: 1 SPI, 2 UART, 3 I2C */
#define SE_GENI_CLK_SEL         0x07c
#define SE_UART_LOOPBACK_CFG    0x22c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_UART_TX_TRANS_CFG    0x25c           /* [1] the CTS line ignored */
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_UART_MANUAL_RFR      0x2ac           /* 0: the RFR line follows the RX FIFO */
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL      0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL      0x634           /* [2] cancel, [1] abort */
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_EN        0x644
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFO         0x700
#define SE_GENI_RX_FIFO         0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804           /* [24:0] words, [31] the last word is partial, [30:28] its bytes */
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24           /* [21:16] the TX FIFO's depth in words */
#define SE_HW_PARAM_1           0xe28           /* [21:16] the RX FIFO's */
#define SE_DMA_GENERAL_CFG      0xe30
#define M_CMD_DONE              (1u << 0)
#define S_CMD_CANCEL            (1u << 4)
#define UART_START_TX           1u
#define UART_START_READ         1u
#define UART_CTS_MASK           (1u << 1)
#define OVERSAMPLING            32u

static unsigned tx_depth = 16, rx_depth = 16;
static uint32_t baud_now;
static bool reading;

static void
say_engine(const char *when)
{
	uint32_t fw = rd(se + GENI_FW_REVISION_RO);

	say("%s: engine 6: firmware %#x (protocol %u, 2 is UART), FIFO %s, depths TX %u RX %u words, status %#x, M irq %#x, S irq %#x, "
	    "RX FIFO %#x\n", when, fw, (fw >> 8) & 0xff, (rd(se + GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "on",
	    (rd(se + SE_HW_PARAM_0) >> 16) & 0x3f, (rd(se + SE_HW_PARAM_1) >> 16) & 0x3f, rd(se + SE_GENI_STATUS),
	    rd(se + SE_GENI_M_IRQ_STATUS), rd(se + SE_GENI_S_IRQ_STATUS), rd(se + SE_GENI_RX_FIFO_STATUS));
}

static bool
uart_set_baud(uint32_t baud)
{
	uint32_t div = S6_CLOCK_HZ / (baud * OVERSAMPLING);

	if (div == 0 || div > 0xfff || div * baud * OVERSAMPLING != S6_CLOCK_HZ) {
		say("uart: %u baud cannot be made of %u Hz\n", baud, S6_CLOCK_HZ);
		return false;
	}
	wr(se + SE_GENI_CLK_SEL, 0);
	wr(se + GENI_SER_M_CLK_CFG, div << 4 | 1);
	wr(se + GENI_SER_S_CLK_CFG, div << 4 | 1);
	baud_now = baud;
	return true;
}

static void
uart_cts(bool obey)
{
	uint32_t cfg = rd(se + SE_UART_TX_TRANS_CFG);

	wr(se + SE_UART_TX_TRANS_CFG, obey ? (cfg & ~UART_CTS_MASK) : (cfg | UART_CTS_MASK));
}

static bool
uart_init(void)
{
	uint32_t fw = rd(se + GENI_FW_REVISION_RO);

	say_engine("uart");
	if (((fw >> 8) & 0xff) != 2 || (rd(se + GENI_IF_DISABLE_RO) & 1)) {
		say("uart: engine 6 is not a UART engine with a FIFO: no Bluetooth\n");
		return false;
	}
	tx_depth = (rd(se + SE_HW_PARAM_0) >> 16) & 0x3f;
	rx_depth = (rd(se + SE_HW_PARAM_1) >> 16) & 0x3f;
	if (tx_depth < 4) {
		tx_depth = 16;
	}
	if (rx_depth < 4) {
		rx_depth = 16;
	}
	wr(se + GENI_CGC_CTRL, rd(se + GENI_CGC_CTRL) | 0x7f);
	wr(se + SE_DMA_GENERAL_CFG, rd(se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(se + GENI_OUTPUT_CTRL, 0x7f);
	wr(se + GENI_FORCE_DEFAULT, 1);
	wr(se + SE_GENI_DMA_MODE_EN, 0);                /* FIFO mode */
	wr(se + SE_GSI_EVENT_EN, 0);
	wr(se + SE_IRQ_EN, 0);                          /* nothing reaches the interrupt controller: polled */
	wr(se + SE_GENI_M_IRQ_EN, 0x3f);                /* (for their status bits) */
	wr(se + SE_GENI_S_IRQ_EN, 0x3f | 3u << 26);
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	/* eight bits a character, four a FIFO word, the first on the wire in the word's low byte, each byte LSB first */
	wr(se + SE_GENI_BYTE_GRAN, 0);
	wr(se + SE_GENI_TX_PACKING_CFG0, 0x0004380e);
	wr(se + SE_GENI_TX_PACKING_CFG1, 0x000c3e0e);
	wr(se + SE_GENI_RX_PACKING_CFG0, 0x0004380e);
	wr(se + SE_GENI_RX_PACKING_CFG1, 0x000c3e0e);
	wr(se + SE_GENI_TX_WATERMARK, 2);
	wr(se + SE_GENI_RX_WATERMARK, 2);
	wr(se + SE_GENI_RX_RFR_WATERMARK, rx_depth - 2);
	/* 8N1, no parity, the RFR line by the FIFO, no loop back */
	wr(se + SE_UART_LOOPBACK_CFG, 0);
	wr(se + SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
	wr(se + SE_UART_RX_TRANS_CFG, 0);
	wr(se + SE_UART_TX_PARITY_CFG, 0);
	wr(se + SE_UART_RX_PARITY_CFG, 0);
	wr(se + SE_UART_TX_WORD_LEN, 8);
	wr(se + SE_UART_RX_WORD_LEN, 8);
	wr(se + SE_UART_TX_STOP_BIT_LEN, 0);
	wr(se + SE_UART_RX_STALE_CNT, 10 * 16);
	wr(se + SE_UART_MANUAL_RFR, 0);
	say("uart: engine 6 set up as a UART: 8N1, FIFO depths %u/%u words, packing %#x %#x\n", tx_depth, rx_depth,
	    rd(se + SE_GENI_TX_PACKING_CFG0), rd(se + SE_GENI_TX_PACKING_CFG1));
	return true;
}

/* ---- what comes in: a ring of bytes, drained from the RX FIFO ---- */
#define RX_RING 8192u
static uint8_t rx_ring[RX_RING];
static unsigned rx_head, rx_count;
static uint32_t rx_overflow, rx_junk;

static void
rx_pump(void)
{
	for (int rounds = 0; rounds < 8; rounds++) {
		uint32_t status = rd(se + SE_GENI_RX_FIFO_STATUS), words = status & 0x1ffffff;
		uint32_t last = (status >> 28) & 7;

		if (words == 0) {
			break;
		}
		for (uint32_t w = 0; w < words; w++) {
			uint32_t word = rd(se + SE_GENI_RX_FIFO);
			unsigned bytes = (w == words - 1 && (status >> 31) && last != 0) ? last : 4;

			for (unsigned b = 0; b < bytes; b++) {
				if (rx_count < RX_RING) {
					rx_ring[(rx_head + rx_count) % RX_RING] = (uint8_t)(word >> (8 * b));
					rx_count++;
				} else {
					rx_overflow++;
				}
			}
		}
		wr(se + SE_GENI_S_IRQ_CLEAR, rd(se + SE_GENI_S_IRQ_STATUS));
	}
}

static void
uart_start_reading(void)
{
	if (!reading) {
		wr(se + SE_GENI_S_IRQ_CLEAR, 0xffffffff);
		wr(se + SE_GENI_S_CMD0, UART_START_READ << 27);
		reading = true;
	}
}

static void
uart_stop_reading(void)
{
	if (!reading) {
		return;
	}
	wr(se + SE_GENI_S_CMD_CTRL, 1u << 2);           /* cancel */
	for (int i = 0; i < 1000 && !(rd(se + SE_GENI_S_IRQ_STATUS) & S_CMD_CANCEL); i++) {
		sleep_us(10);
	}
	if (rd(se + SE_GENI_STATUS) & (1u << 12)) {
		wr(se + SE_GENI_S_CMD_CTRL, 1u << 1);   /* abort */
		sleep_us(100);
	}
	rx_pump();
	wr(se + SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	reading = false;
}

/* bytes to the chip; 0 or ETIMEDOUT (the CTS line held, or the engine stuck) */
static int
uart_write(const uint8_t *bytes, unsigned length)
{
	uint32_t since = ms_now(), limit = 200 + (uint32_t)((uint64_t)length * 10 * 1000 * 3 / (baud_now ? baud_now : 2400));
	unsigned i = 0;

	if (length == 0) {
		return 0;
	}
	while (rd(se + SE_GENI_STATUS) & 1) {
		if (ms_now() - since > 100) {
			wr(se + SE_GENI_M_CMD_CTRL, 1u << 1);   /* abort what is stuck */
			sleep_us(100);
			break;
		}
		rx_pump();
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_UART_TX_TRANS_LEN, length);
	wr(se + SE_GENI_M_CMD0, UART_START_TX << 27);
	while (i < length) {
		if ((rd(se + SE_GENI_TX_FIFO_STATUS) & 0x0fffffff) < tx_depth) {
			uint32_t word = 0;

			for (unsigned b = 0; b < 4 && i < length; b++, i++) {
				word |= (uint32_t)bytes[i] << (8 * b);
			}
			wr(se + SE_GENI_TX_FIFO, word);
			continue;
		}
		if (reading) {
			rx_pump();
		}
		if (ms_now() - since > limit) {
			wr(se + SE_GENI_M_CMD_CTRL, 1u << 1);
			return ETIMEDOUT;
		}
	}
	while (!(rd(se + SE_GENI_M_IRQ_STATUS) & M_CMD_DONE)) {
		if (reading) {
			rx_pump();
		}
		if (ms_now() - since > limit) {
			wr(se + SE_GENI_M_CMD_CTRL, 1u << 1);
			return ETIMEDOUT;
		}
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	return 0;
}

/* ---- H4 over the UART ---- */
static unsigned
h4_length_at(unsigned offset)
{
	uint8_t b[5];
	unsigned have = rx_count - offset < 5 ? rx_count - offset : 5;

	for (unsigned i = 0; i < have; i++) {
		b[i] = rx_ring[(rx_head + offset + i) % RX_RING];
	}
	if (have < 1) {
		return 0;
	}
	switch (b[0]) {
	case 2: return have >= 5 ? 5u + (b[3] | ((unsigned)b[4] << 8)) : 0;
	case 3: return have >= 4 ? 4u + b[3] : 0;
	case 4: return have >= 3 ? 3u + b[2] : 0;
	case 5: return have >= 5 ? 5u + ((b[3] | ((unsigned)b[4] << 8)) & 0x3fff) : 0;
	default: return 0;
	}
}

/* one whole H4 packet from the chip into h4: its length, 0 if none came within wait_ms */
static int
bt_receive(uint8_t *h4, unsigned room, unsigned wait_ms)
{
	uint32_t since = ms_now();

	for (;;) {
		unsigned n;

		rx_pump();
		while (rx_count > 0) {
			uint8_t type = rx_ring[rx_head];

			if (type >= 2 && type <= 5) {
				break;
			}
			if (rx_junk++ < 16) {
				say("uart: byte %02x where a packet should begin: skipped\n", type);
			}
			rx_head = (rx_head + 1) % RX_RING;
			rx_count--;
		}
		n = h4_length_at(0);
		if (n != 0 && n <= rx_count) {
			if (n > room) {
				say("uart: a packet of %u bytes, room for %u: dropped\n", n, room);
				rx_head = (rx_head + n) % RX_RING;
				rx_count -= n;
				continue;
			}
			for (unsigned i = 0; i < n; i++) {
				h4[i] = rx_ring[(rx_head + i) % RX_RING];
			}
			rx_head = (rx_head + n) % RX_RING;
			rx_count -= n;
			return (int)n;
		}
		if (ms_now() - since >= wait_ms) {
			return 0;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
}

static int
bt_send(const uint8_t *h4, unsigned length)
{
	return uart_write(h4, length);
}

/* ---- the chip ---- */
static uint8_t packet[1100];
static uint32_t said_events;

static void
say_packet(const char *what, const uint8_t *p, unsigned length)
{
	char text[3 * 24 + 1];
	unsigned n = length < 24 ? length : 24;

	for (unsigned i = 0; i < n; i++) {
		snprintf(text + 3 * i, 4, " %02x", p[i]);
	}
	text[3 * n] = 0;
	say("%s %u bytes:%s%s\n", what, length, text, length > n ? " ..." : "");
}

/* a vendor command 0xfc00-style: 01 <opcode> <length> <parameters> */
static int
command(uint16_t opcode, const uint8_t *parameters, unsigned length)
{
	uint8_t c[4 + 255];

	c[0] = 0x01;
	c[1] = (uint8_t)opcode;
	c[2] = (uint8_t)(opcode >> 8);
	c[3] = (uint8_t)length;
	if (length != 0) {
		memcpy(c + 4, parameters, length);
	}
	return uart_write(c, 4 + length);
}

/* waits for an event: the vendor event (0xff) whose first two bytes are `cresp rtype`, or a Command Complete of `opcode` */
static int
wait_event(bool vendor, uint8_t rtype, uint16_t opcode, unsigned wait_ms)
{
	uint32_t since = ms_now();

	while (ms_now() - since < wait_ms) {
		int n = bt_receive(packet, sizeof(packet), 10);

		if (n <= 0) {
			continue;
		}
		if (vendor && n >= 5 && packet[0] == 0x04 && packet[1] == 0xff && packet[3] == 0x00 && packet[4] == rtype) {
			return n;
		}
		if (!vendor && n >= 7 && packet[0] == 0x04 && packet[1] == 0x0e && packet[4] == (uint8_t)opcode && packet[5] == (uint8_t)(opcode >> 8)) {
			return n;
		}
		if (said_events++ < 24) {
			say_packet("event (not the one waited for)", packet, (unsigned)n);
		}
	}
	return 0;
}

static void
pulse(uint8_t byte, uint32_t baud, uint32_t after_us)
{
	uart_cts(false);
	uart_set_baud(baud);
	say("chip: the byte %#x at %u baud, the CTS line ignored\n", byte, baud);
	(void)uart_write(&byte, 1);
	sleep_us(10000 * 2400 / baud + 2000);           /* the byte out of the shift register */
	sleep_us(after_us);
}

static void
chip_off(void)
{
	uart_stop_reading();
	say("chip: off (0xc0 at 2400 baud)\n");
	pulse(0xc0, 2400, 10000);
	uart_set_baud(115200);
}

static struct {
	uint32_t product, soc;
	uint16_t patch, rom;
	uint8_t rom_ver;
} chipver;

static bool
chip_on(void)
{
	static const uint8_t ask_version[1] = { 0x19 };
	int n;

	pulse(0xc0, 2400, 10000);                       /* into its boot mode */
	pulse(0xfc, 115200, 100000);                    /* on: it boots */
	rx_head = rx_count = 0;
	uart_start_reading();
	uart_cts(true);
	say("chip: the version (vendor command 0xfc00, 0x19)\n");
	if (command(0xfc00, ask_version, 1) != 0) {
		say("chip: the command could not be sent in time (the CTS line held?)\n");
		return false;
	}
	n = wait_event(true, 0x02, 0, 2000);
	if (n < 5 + 12) {
		say("chip: no version came in 2 s (%d bytes); %u bytes waiting, %u skipped\n", n, rx_count, rx_junk);
		say_engine("chip");
		return false;
	}
	say_packet("chip: the version event", packet, (unsigned)n);
	chipver.product = packet[5] | packet[6] << 8 | packet[7] << 16 | (uint32_t)packet[8] << 24;
	chipver.patch = (uint16_t)(packet[9] | packet[10] << 8);
	chipver.rom = (uint16_t)(packet[11] | packet[12] << 8);
	chipver.soc = packet[13] | packet[14] << 8 | packet[15] << 16 | (uint32_t)packet[16] << 24;
	{
		uint32_t soc_ver = chipver.soc << 16 | chipver.rom;

		chipver.rom_ver = (uint8_t)(((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf));
	}
	say("chip: THE WCN3990 ANSWERS: product %#x, patch %#x, ROM %#x, SoC %#x: its files are crbtfw%02x.tlv and crnv%02x.bin\n",
	    chipver.product, chipver.patch, chipver.rom, chipver.soc, chipver.rom_ver, chipver.rom_ver);
	return chipver.soc != 0 && chipver.rom != 0;
}

/* one of Qualcomm's TLV files to the chip, in pieces of at most 243 bytes */
static bool
download(const char *what, const uint8_t *data, unsigned size, uint8_t mode)
{
	unsigned at = 0, pieces = 0, tenth = 0;
	uint32_t since = ms_now();

	say("download: %s, %u bytes, download mode %u (%s)\n", what, size, mode,
	    mode == 0 ? "each piece answered" : mode == 1 ? "no vendor events" : mode == 2 ? "no Command Completes" : "only the last answered");
	while (at < size) {
		uint8_t c[2 + 243];
		unsigned piece = size - at < 243 ? size - at : 243;
		bool last = at + piece >= size;
		int n;

		c[0] = 0x1e;
		c[1] = (uint8_t)piece;
		memcpy(c + 2, data + at, piece);
		if (command(0xfc00, c, 2 + piece) != 0) {
			say("download: %s: piece %u (at %u) could not be sent\n", what, pieces, at);
			return false;
		}
		if (mode == 0 || mode == 2 || last || piece < 243) {
			n = wait_event(true, 0x04, 0, 3000);
			if (n < 6) {
				say("download: %s: piece %u (at %u): no answer in 3 s\n", what, pieces, at);
				return false;
			}
			if (packet[5] != 0) {
				say("download: %s: piece %u (at %u): the chip says %#x\n", what, pieces, at, packet[5]);
				return false;
			}
		} else {
			while (bt_receive(packet, sizeof(packet), 0) > 0) {
				/* Command Completes of pieces nobody waits for */
			}
		}
		at += piece;
		pieces++;
		if (at * 10 / size > tenth) {
			tenth = at * 10 / size;
			if (tenth % 2 == 0) {
				say("download: %s: %u%% (%u pieces, %u ms)\n", what, tenth * 10, pieces, ms_now() - since);
			}
		}
	}
	say("download: %s: done, %u pieces in %u ms\n", what, pieces, ms_now() - since);
	return true;
}

/* the NVM's tags changed for this driver (see the head); false if the file is not what it should be */
static bool
nvm_edit(uint8_t *data, unsigned size, uint8_t baud_index)
{
	unsigned at = 0, length, type, idx = 0;

	if (size < 4) {
		return false;
	}
	type = data[0];
	length = data[1] | data[2] << 8 | (unsigned)data[3] << 16;
	if (type == 4) {
		if (size < 8) {
			return false;
		}
		at = 4;
		type = data[4];
		length = data[5] | data[6] << 8 | (unsigned)data[7] << 16;
	}
	at += 4;
	say("nvm: type %u, %u bytes of tags\n", type, length);
	if (type != 2 || at + length > size) {
		say("nvm: not a set of tags this driver knows\n");
		return false;
	}
	while (idx + 12 <= length) {
		uint8_t *tag = data + at + idx;
		unsigned id = tag[0] | tag[1] << 8, len = tag[2] | tag[3] << 8;
		uint8_t *value = tag + 12;

		if (idx + 12 + len > length) {
			return false;
		}
		if (id == 2 && len == 6) {
			say("nvm: tag 2, the address: %02x:%02x:%02x:%02x:%02x:%02x\n", value[5], value[4], value[3], value[2], value[1], value[0]);
		} else if (id == 17 && len >= 3) {
			say("nvm: tag 17, the transport: %02x %02x %02x -> in-band sleep off, speed %u\n", value[0], value[1], value[2], baud_index);
			value[0] &= (uint8_t)~0x80;
			value[2] = baud_index;
		} else if (id == 27 && len >= 1) {
			say("nvm: tag 27, deep sleep: %02x -> off\n", value[0]);
			value[0] &= (uint8_t)~0x01;
		}
		idx += 12 + len;
	}
	return true;
}

/*
 * A file of /fw on the root file system, read whole into memory of ours (the caller gives it back). The system image keeps
 * it compressed, which the file system undoes and a read of the image's memory would not (hakios.mpss845.c's root_file);
 * so it waits for the root to be mounted, at most 60 s.
 */
static uint8_t *
firmware(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	for (int i = 0; i < 600 && rootvnode == NULLVP; i++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (rootvnode == NULLVP || vnode_lookup(path, 0, &vnode, context) != 0) {
		say("firmware: no %s on the root: put the phone's partition \"bluetooth\"'s IMAGE/ files into ../fw-bt/ "
		    "(lower case) and run BT=1 SYSTEM=1 userland/build.sh\n", path);
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size < 16 || attributes.va_data_size > 1024 * 1024 ||
	    (data = kalloc_data((vm_size_t)attributes.va_data_size, Z_WAITOK)) == NULL) {
		vnode_put(vnode);
		say("firmware: %s: not a size this driver takes\n", path);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		say("firmware: %s: read error %d, %d bytes left\n", path, error, left);
		kfree_data(data, (vm_size_t)attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	say("firmware: %s, %llu bytes\n", path, (unsigned long long)*size);
	return data;
}

/* aa:bb:cc:dd:ee:ff into six bytes, in that order */
static bool
parse_mac(const char *text, uint8_t out[6])
{
	unsigned count = 0, digits = 0, value = 0;

	for (const char *c = text;; c++) {
		int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;

		if (digit >= 0 && digits < 2) {
			value = value << 4 | (unsigned)digit;
			digits++;
		} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
			out[count++] = (uint8_t)value;
			digits = value = 0;
		} else {
			return false;
		}
		if (*c == 0) {
			return count == 6;
		}
	}
}

static bool
address(uint8_t out[6])
{
	char text[32] = "";

	if (PE_parse_boot_arg_str("bt_mac", text, sizeof(text)) && parse_mac(text, out)) {
		return true;
	}
	if (PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) && parse_mac(text, out)) {
		out[5] = (uint8_t)(out[5] + 1);
		return true;
	}
	return false;
}

static bool
chip_setup(uint8_t bt_address[6], bool *have_address)
{
	char name[32];
	uint8_t *patch, *nvm;
	uint64_t patch_size = 0, nvm_size = 0;
	int n;
	bool ok;

	snprintf(name, sizeof(name), "crbtfw%02x.tlv", chipver.rom_ver);
	if ((patch = firmware(name, &patch_size)) == NULL) {
		return false;
	}
	ok = patch_size >= 24 && patch[0] == 1;
	if (!ok) {
		say("firmware: %s is not a rampatch (type %u)\n", name, patch[0]);
	}
	ok = ok && download(name, patch, (unsigned)patch_size, patch[14]);
	kfree_data(patch, (vm_size_t)patch_size);
	if (!ok) {
		return false;
	}
	sleep_us(10000);                                /* (Linux gives the chip 10 ms before the NVM) */
	snprintf(name, sizeof(name), "crnv%02x.bin", chipver.rom_ver);
	if ((nvm = firmware(name, &nvm_size)) == NULL) {
		return false;
	}
	ok = nvm_edit(nvm, (unsigned)nvm_size, 0 /* 115200 */) && download(name, nvm, (unsigned)nvm_size, 0);
	kfree_data(nvm, (vm_size_t)nvm_size);
	if (!ok) {
		return false;
	}
	say("chip: HCI_Reset\n");
	if (command(0x0c03, NULL, 0) != 0 || (n = wait_event(false, 0, 0x0c03, 5000)) < 7 || packet[6] != 0) {
		say("chip: no good answer to HCI_Reset\n");
		return false;
	}
	*have_address = address(bt_address);
	if (*have_address) {
		say("chip: the address %02x:%02x:%02x:%02x:%02x:%02x (vendor command 0xfc14)\n", bt_address[0], bt_address[1], bt_address[2],
		    bt_address[3], bt_address[4], bt_address[5]);
		if (command(0xfc14, bt_address, 6) != 0 || (n = wait_event(false, 0, 0xfc14, 2000)) < 7 || packet[6] != 0) {
			say("chip: the address was not taken (%d bytes%s): the NVM's stays\n", n, n >= 7 ? ", a status" : "");
			*have_address = false;
		}
	} else {
		say("chip: no bt_mac nor wifi_mac: the NVM's address stays, and iOS is given none\n");
	}
	return true;
}

/* ---- the run ---- */
static uint32_t level;
static uint32_t after_seconds = 30;
static thread_call_t start_call;

static bool
map_all(void)
{
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm_east = ml_io_map(TLMM_EAST_PHYS, TLMM_EAST_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (gcc == 0 || tlmm_east == 0 || se == 0) {
		say("could not map the GCC, the TLMM or the engine\n");
		return false;
	}
	return true;
}

static void
bt_thread(__unused void *arg, __unused wait_result_t w)
{
	uint8_t bt_address[6] = { 0 };
	bool have_address = false;

	if (!map_all()) {
		goto out;
	}
	say("level %u: the WCN3990's Bluetooth on serial engine 6 (UART6)\n", level);
	say_clocks("look");
	say_pins("look");
	if (clocks_running()) {
		say_engine("look");
	} else {
		say("look: engine 6's registers not read: its clocks are off (a read could hang the bus)\n");
	}
	if (level < 2) {
		goto out;
	}
	if (!clocks_on() || !uart_init()) {
		goto out;
	}
	pins_to_uart();
	if (!chip_on()) {
		chip_off();
		goto out;
	}
	if (level < 3) {
		chip_off();
		goto out;
	}
	if (!chip_setup(bt_address, &have_address)) {
		chip_off();
		goto out;
	}
	if (level < 4) {
		(void)hakios_bthci_start("wcn3990", bt_send, bt_receive, chip_off, NULL, false);
		say("level 3 done: rx overflow %u, bytes skipped %u\n", rx_overflow, rx_junk);
		chip_off();
		goto out;
	}
	if (!hakios_bthci_start("wcn3990", bt_send, bt_receive, chip_off, have_address ? bt_address : NULL, true)) {
		chip_off();
	}
out:
	say("the end of the run's Bluetooth\n");
	thread_terminate(current_thread());
}

static void
start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start_priority(bt_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

__startup_func
static void
bt3990_init(void)
{
	uint32_t value;
	uint64_t deadline;

	if (!PE_parse_boot_argn("bt", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (PE_parse_boot_argn("bt_after", &value, sizeof(value)) && value <= 3600) {
		after_seconds = value;
	}
	start_call = thread_call_allocate(start, NULL);
	clock_interval_to_deadline(after_seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(start_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt3990_init);
