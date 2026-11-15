/*
 * What the drivers share, kept apart from any one of them, so that each can be left out of the kernel on its own
 * (drivers/<subsystem>/conf, CONFIG_HAKIOS_*: docs/drivers-config.md).
 *
 *   the locks      of the hardware several drivers take turns at: the PMIC arbiter (the buttons, the flashlight, the
 *                  camera's regulators, the PMIC survey), the always-on processor's mailbox, the shared memory's
 *                  allocation and RPMh's sequencer (the Wi-Fi, audio DSP and modem bring-ups, the touch screen's
 *                  supply), and the SMMU's free match registers (Wi-Fi, sound, camera, modem). They were in
 *                  hakios.buttons.c and hakios.wifi.c, which the others then needed in the kernel to link.
 *   kern.hakios    the sysctl node the drivers hang theirs under (it was hakios.battery.c's)
 *   the list       of the drivers this kernel was built with: sysctl kern.hakios.drivers, and a line in the boot log,
 *                  so that a run says what was in it (hakios.drivers.h is made by tools/hakios-config.py kernel).
 */
#include <kern/locks.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios.drivers.h"

/* kern.hakios: the node every driver of this port hangs its sysctls under (it was hakios.battery.c's) */
SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");

static LCK_GRP_DECLARE(hakios_platform_lock_group, "hakios-platform");

/*
 * One transfer at a time through the PMIC arbiter for this system, whoever asks: with two at once the arbiter drops
 * one ("the transfer was dropped"), as Linux never lets happen (one lock for the bus). A spin lock: it is taken
 * around a transfer of some tens of microseconds.
 */
static LCK_SPIN_DECLARE(spmi_lock, &hakios_platform_lock_group);

void
hakios_spmi_lock(void)
{
	lck_spin_lock(&spmi_lock);
}

void
hakios_spmi_unlock(void)
{
	lck_spin_unlock(&spmi_lock);
}

/*
 * A read of any PMIC peripheral's registers for the drivers that do not map the arbiter themselves (the touch screen's
 * charger bit): set by drivers/input/hakios.buttons.c once it has the arbiter (buttons=1); NULL before, or without it.
 */
int (*volatile hakios_pmic_reader)(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count);

/*
 * What the DSPs' starts use while another may be at work beside them: the mailbox to the always-on processor, one
 * message at a time, and the allocation of items in the shared memory (whose hardware lock both take under the same
 * number, so it does not keep the two apart); RPMh's sequencer (TCS 0 of the RSC); a free match register and context
 * of the SMMU, found and taken.
 */
LCK_MTX_DECLARE(hakios_aop_mailbox_lock, &hakios_platform_lock_group);
LCK_MTX_DECLARE(hakios_smem_alloc_lock, &hakios_platform_lock_group);
LCK_MTX_DECLARE(hakios_rsc_lock, &hakios_platform_lock_group);
LCK_MTX_DECLARE(hakios_smmu_lock, &hakios_platform_lock_group);

/*
 * The levels of RPMh's rails that more than one driver votes for in DRV 2 (cx.lvl and mx.lvl: the modem's boot, the
 * audio DSP's boot, the cameras): RPMh keeps one value per DRV and resource, the last written, so each of them writes
 * what this gives -- the highest of every driver's own wish for that resource, its own just recorded -- and not its own
 * alone: one giving its vote back (0) does not take another's away. Called with hakios_rsc_lock held (or on the way out
 * of a panic, when nothing else runs). Clients: 0 the modem (hakios.mpss845.c), 1 the audio DSP (hakios.adsp845.c),
 * 2 the cameras (hakios.camera845.c).
 */
#define HAKIOS_RPMH_CLIENTS     3
#define RPMH_SHARED_SLOTS       4
static struct {
	uint32_t        address;
	uint32_t        wish[HAKIOS_RPMH_CLIENTS];
} rpmh_shared[RPMH_SHARED_SLOTS];

uint32_t hakios_rpmh_shared(unsigned client, uint32_t address, uint32_t value);
uint32_t
hakios_rpmh_shared(unsigned client, uint32_t address, uint32_t value)
{
	unsigned s, free = RPMH_SHARED_SLOTS;
	uint32_t top = 0;

	if (client >= HAKIOS_RPMH_CLIENTS || address == 0) {
		return value;
	}
	for (s = 0; s < RPMH_SHARED_SLOTS && rpmh_shared[s].address != address; s++) {
		if (rpmh_shared[s].address == 0 && free == RPMH_SHARED_SLOTS) {
			free = s;
		}
	}
	if (s == RPMH_SHARED_SLOTS) {
		if (free == RPMH_SHARED_SLOTS) {
			return value;           /* (no room: the driver's own, as before there was this) */
		}
		s = free;
		rpmh_shared[s].address = address;
	}
	rpmh_shared[s].wish[client] = value;
	for (unsigned c = 0; c < HAKIOS_RPMH_CLIENTS; c++) {
		top = rpmh_shared[s].wish[c] > top ? rpmh_shared[s].wish[c] : top;
	}
	return top;
}

static char hakios_drivers_string[] = HAKIOS_DRIVERS_LIST;

SYSCTL_STRING(_kern_hakios, OID_AUTO, drivers, CTLFLAG_RD | CTLFLAG_LOCKED, hakios_drivers_string, 0,
    "the drivers this kernel was built with (CONFIG_HAKIOS_* of its configuration, lowercase)");

static void
hakios_platform_init(void)
{
	printf("hakios-kernel: %d drivers built in (%s): %s\n", HAKIOS_DRIVERS_COUNT, HAKIOS_DRIVERS_CONFIG, hakios_drivers_string);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_platform_init);
