/*
 * The OnePlus 6T's screen brightness: its Samsung AMOLED panel (s6e3fc2x01, Linux's panel-samsung-s6e3fc2x01.c) has no
 * backlight of its own to dim; it is told its brightness by a DCS command over DSI, set_display_brightness (0x51)
 * with a 16-bit value, 0 to 1023, sent in high speed. The boot loader leaves the panel on, in video mode, with its
 * brightness control enabled (0x53 0x20); nothing else of the display is touched here. The 6T's own boot loader drives
 * this panel in COMMAND mode (ctrl 0x1f5: CMD_MODE_EN, where Linux has video mode), and sends its own commands the same
 * way, by physical address (dma_base 0x9c0e1000, 8 bytes, in low power: read 11-11).
 *
 * The command goes through DSI 0's command DMA, as Linux's dsi_host.c sends it (dsi_cmd_dma_add: the MSM layout of a
 * long packet in memory; msm_dsi_host_cmd_xfer_commit: DMA_BASE, DMA_LEN, TRIG_DMA; the CMD_DMA_DONE bit). The
 * controller is the "6G" kind, whose registers are 4 bytes further on than dsi.xml has them. Where Linux gives the
 * DMA an address in the display's SMMU space, the boot loader's display reads the splash memory by its physical
 * address (0x9d400000, 36 MB: the phone's device tree, cont_splash_region), so the packet goes into the last megabyte
 * of that, where no picture is.
 *
 * Boot-args: dsibl=1 reads the controller and takes requests (sysctl kern.hakios.brightness, 0..1023);
 * dsibl_test=1 also runs a sequence a person can see (dim, bright, middle) a few seconds after the start.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define DSI0_PHYS               0x0ae94000UL    /* sdm845.dtsi mdss_dsi0 */
#define DSI_SIZE                0x400UL
#define R(x)                    ((x) + 4)       /* the 6G controller's shift (dsi_host.c: DSI_6G_REG_SHIFT) */
#define DSI_CTRL                R(0x000)
#define DSI_STATUS0             R(0x004)
#define DSI_FIFO_STATUS         R(0x008)
#define DSI_VID_CFG0            R(0x00c)
#define DSI_CMD_DMA_CTRL        R(0x038)
#define DSI_CMD_CFG0            R(0x03c)
#define DSI_DMA_BASE            R(0x044)
#define DSI_DMA_LEN             R(0x048)
#define DSI_TRIG_CTRL           R(0x080)
#define DSI_TRIG_DMA            R(0x08c)
#define DSI_LANE_STATUS         R(0x0a4)
#define DSI_LANE_CTRL           R(0x0a8)
#define DSI_ERR_INT_MASK0       R(0x108)
#define DSI_INTR_CTRL           R(0x10c)
#define DSI_CLK_CTRL            R(0x118)
#define DSI_VERSION             R(0x1f0)
#define CTRL_ENABLE             (1u << 0)
#define CTRL_VID_MODE_EN        (1u << 1)
#define CTRL_CMD_MODE_EN        (1u << 2)
#define CMD_DMA_FROM_FB         (1u << 28)      /* the DMA reads memory */
#define CMD_DMA_LOW_POWER       (1u << 26)
#define IRQ_CMD_DMA_DONE        (1u << 0)
#define IRQ_MASK_CMD_DMA_DONE   (1u << 1)

#define PACKET_PHYS             (0x9d400000UL + 0x2300000UL)    /* the splash memory's last megabyte but one */
#define PACKET_SIZE             0x1000UL

static vm_offset_t dsi, packet;
static int brightness = -1;                     /* the last value sent, -1 before any */
static uint32_t sends, failures;
static LCK_GRP_DECLARE(dsibl_group, "hakios-dsibl");
static LCK_MTX_DECLARE(dsibl_lock, &dsibl_group);

static inline uint32_t
rd(uint32_t offset)
{
	return *(volatile uint32_t *)(dsi + offset);
}

static inline void
wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(dsi + offset) = value;
}

/* One set_display_brightness, 0..1023, as a DCS long write; true if the controller said it was sent. */
static bool
dsibl_send(uint32_t value)
{
	volatile uint8_t *p = (volatile uint8_t *)packet;
	uint32_t intr, done = 0;
	uint64_t start = mach_absolute_time(), ns;

	if (value > 1023) {
		value = 1023;
	}
	/* the MSM layout (dsi_cmd_dma_add): word count low and high, data type 0x39, "last, long"; payload; 0xff to a word */
	p[0] = 3;
	p[1] = 0;
	p[2] = 0x39;
	p[3] = 0x80 | 0x40;
	p[4] = 0x51;
	p[5] = (uint8_t)(value >> 8);
	p[6] = (uint8_t)value;
	p[7] = 0xff;
	__asm__ volatile ("dsb sy" ::: "memory");

	wr(DSI_CMD_DMA_CTRL, rd(DSI_CMD_DMA_CTRL) | CMD_DMA_FROM_FB);  /* low power or not, as the boot loader sends (low power) */
	intr = rd(DSI_INTR_CTRL);
	wr(DSI_INTR_CTRL, intr | IRQ_MASK_CMD_DMA_DONE);        /* (writing the status bit back clears it) */
	wr(DSI_DMA_BASE, (uint32_t)PACKET_PHYS);
	wr(DSI_DMA_LEN, 8);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(DSI_TRIG_DMA, 1);
	for (int i = 0; i < 2000 && !(done & IRQ_CMD_DMA_DONE); i++) {
		delay_for_interval(50, NSEC_PER_USEC);
		done = rd(DSI_INTR_CTRL);
	}
	wr(DSI_INTR_CTRL, (done & IRQ_CMD_DMA_DONE) | (intr & IRQ_MASK_CMD_DMA_DONE));        /* clear; the mask as it was */
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	sends++;
	if (!(done & IRQ_CMD_DMA_DONE)) {
		failures++;
		printf("dsibl: brightness %u: not sent (intr %#x, status0 %#x, fifo %#x)\n", value, done, rd(DSI_STATUS0),
		    rd(DSI_FIFO_STATUS));
		return false;
	}
	brightness = (int)value;
	if (sends <= 8 || sends % 50 == 0) {
		printf("dsibl: brightness %u sent in %llu us (%u sent, %u failed)\n", value, ns / 1000, sends, failures);
	}
	return true;
}

static int
sysctl_brightness SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = brightness, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (dsi == 0) {
		return ENODEV;
	}
	if (value < 0 || value > 1023) {
		return EINVAL;
	}
	lck_mtx_lock(&dsibl_lock);
	error = dsibl_send((uint32_t)value) ? 0 : EIO;
	lck_mtx_unlock(&dsibl_lock);
	return error;
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    sysctl_brightness, "I", "the panel's brightness, 0..1023 (the s6e3fc2x01's set_display_brightness); -1 before any is set");

static void
dsibl_test(__unused void *parameter, __unused wait_result_t result)
{
	static const uint32_t steps[] = { 20, 1023, 20, 1023, 184 };     /* (184: what the boot loader leaves)
 */

	delay_for_interval(8000, NSEC_PER_MSEC);
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		printf("dsibl: test: brightness %u\n", steps[i]);
		lck_mtx_lock(&dsibl_lock);
		(void)dsibl_send(steps[i]);
		lck_mtx_unlock(&dsibl_lock);
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
hakios_dsibl_init(void)
{
	uint32_t on = 0, ctrl;
	thread_t thread;

	if (!PE_parse_boot_argn("dsibl", &on, sizeof(on)) || !on) {
		return;
	}
	dsi = ml_io_map(DSI0_PHYS, DSI_SIZE);
	ctrl = rd(DSI_CTRL);
	printf("dsibl: DSI 0: version %#x/%#x, ctrl %#x, status0 %#x, fifo %#x, vid_cfg0 %#x, cmd_dma_ctrl %#x, cmd_cfg0 %#x\n",
	    *(volatile uint32_t *)dsi, rd(DSI_VERSION), ctrl, rd(DSI_STATUS0), rd(DSI_FIFO_STATUS), rd(DSI_VID_CFG0),
	    rd(DSI_CMD_DMA_CTRL), rd(DSI_CMD_CFG0));
	printf("dsibl: dma_base %#x, dma_len %#x, trig_ctrl %#x, lane_status %#x, lane_ctrl %#x, intr %#x, err_mask %#x, "
	    "clk_ctrl %#x\n", rd(DSI_DMA_BASE), rd(DSI_DMA_LEN), rd(DSI_TRIG_CTRL), rd(DSI_LANE_STATUS), rd(DSI_LANE_CTRL),
	    rd(DSI_INTR_CTRL), rd(DSI_ERR_INT_MASK0), rd(DSI_CLK_CTRL));
	if (!(ctrl & CTRL_ENABLE) || !(ctrl & (CTRL_VID_MODE_EN | CTRL_CMD_MODE_EN))) {
		printf("dsibl: the controller is not on: nothing is sent\n");
		dsi = 0;
		return;
	}
	{
		/* what the boot loader sent last, where it left it (its DMA_BASE, DMA_LEN): for comparison */
		uint32_t base = rd(DSI_DMA_BASE), len = rd(DSI_DMA_LEN);

		if (base >= 0x80000000u && len > 0 && len <= 64) {
			volatile uint8_t *last = (volatile uint8_t *)ml_io_map(base & ~0xfffu, 0x1000) + (base & 0xfff);

			printf("dsibl: the boot loader's last command, %u bytes at %#x: %02x %02x %02x %02x %02x %02x %02x %02x\n", len,
			    base, last[0], last[1], last[2], last[3], last[4], last[5], last[6], last[7]);
		}
	}
	packet = ml_io_map(PACKET_PHYS, PACKET_SIZE);
	printf("dsibl: the panel's brightness by DCS 0x51 (%s mode), packets at %#lx; sysctl kern.hakios.brightness\n",
	    (ctrl & CTRL_CMD_MODE_EN) ? "command" : "video", PACKET_PHYS);
	on = 0;
	if (PE_parse_boot_argn("dsibl_test", &on, sizeof(on)) && on &&
	    kernel_thread_start(dsibl_test, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
