/*
 * /dev/hakios-hid: what Bluetooth's HID devices send, for backboardd to make events of (driver HIDRESOURCE).
 *
 * bluetoothd makes a HID device of a keyboard or a mouse it connects to (HID over GATT) through IOHIDFamily's
 * IOHIDUserDevice: the service "IOHIDResource" and its user client (iokit/Kernel/hakios.hidresource.cpp). This kernel
 * has no IOHIDFamily; that file stands in for the user client, and gives what it is handed to this queue: a device
 * made (its report descriptor), each input report, a device gone. backboardd reads it (userland/src/inject.c,
 * hid_thread()) and posts the keys through its virtual services, as it does the phone's own buttons.
 * A record is a header (type 1 made, 2 a report, 3 gone; the device's number; the length) and that many bytes.
 * The 6T, run 91: an ESP32 as a BLE keyboard paired and connected, and typed nothing.
 */
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <pexpert/pexpert.h>

extern void HakiosHIDResourceStart(void);      /* hakios.hidresource.cpp */
extern void loader_log_putc(char c);

struct hidres_record {
	uint16_t        type;
	uint16_t        device;
	uint32_t        length;
};

#define RING_SIZE       32768u
static uint8_t ring[RING_SIZE];
static uint32_t ring_head, ring_tail;          /* bytes written, read (free-running) */
static uint32_t dropped;
static LCK_GRP_DECLARE(hidres_group, "hakios-hid");
static LCK_MTX_DECLARE(hidres_lock, &hidres_group);

void hakios_hidres_say(const char *format, ...) __printflike(1, 2);

void
hakios_hidres_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

static void
ring_put(const void *bytes, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		ring[(ring_head + i) % RING_SIZE] = ((const uint8_t *)bytes)[i];
	}
	ring_head += length;
}

/* From the user client: a record queued, or dropped when the reader is too far behind. */
void hakios_hidres_record(uint16_t type, uint16_t device, const void *data, uint32_t length);

void
hakios_hidres_record(uint16_t type, uint16_t device, const void *data, uint32_t length)
{
	struct hidres_record header = { type, device, length };

	if (length > 4096) {
		return;
	}
	lck_mtx_lock(&hidres_lock);
	if (RING_SIZE - (ring_head - ring_tail) < sizeof(header) + length) {
		dropped++;
	} else {
		ring_put(&header, sizeof(header));
		ring_put(data, length);
	}
	lck_mtx_unlock(&hidres_lock);
	wakeup(ring);
}

static int
hid_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	hakios_hidres_say("hidres: /dev/hakios-hid opened by %s (pid %d)\n", name, proc_selfpid());
	return 0;
}

static int
hid_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

/* whole records only, as many as fit; waits for one */
static int
hid_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int error = 0;
	bool gave = false;

	lck_mtx_lock(&hidres_lock);
	while (ring_head == ring_tail) {
		error = msleep(ring, &hidres_lock, PCATCH | PZERO, "hakios-hid", NULL);
		if (error) {
			lck_mtx_unlock(&hidres_lock);
			return error;
		}
	}
	while (ring_head != ring_tail) {
		struct hidres_record header;
		uint8_t record[sizeof(header) + 4096];
		uint32_t whole;

		for (uint32_t i = 0; i < sizeof(header); i++) {
			((uint8_t *)&header)[i] = ring[(ring_tail + i) % RING_SIZE];
		}
		whole = (uint32_t)sizeof(header) + header.length;
		if ((user_ssize_t)whole > uio_resid(uio)) {
			if (!gave) {
				error = EMSGSIZE;
			}
			break;
		}
		for (uint32_t i = 0; i < whole; i++) {
			record[i] = ring[(ring_tail + i) % RING_SIZE];
		}
		ring_tail += whole;
		lck_mtx_unlock(&hidres_lock);
		error = uiomove((caddr_t)record, (int)whole, uio);
		lck_mtx_lock(&hidres_lock);
		if (error) {
			break;
		}
		gave = true;
	}
	lck_mtx_unlock(&hidres_lock);
	return error;
}

static const struct cdevsw hid_cdevsw = {
	.d_open = hid_open, .d_close = hid_close, .d_read = hid_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static thread_call_t start_call;

static void
hidres_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	int major = cdevsw_add(-1, &hid_cdevsw);

	/* (0666 as /dev/hakios-touch: backboardd is not root; what a keyboard types goes through here) */
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-hid") == NULL) {
		hakios_hidres_say("hidres: /dev/hakios-hid could not be made\n");
	}
	HakiosHIDResourceStart();
}

/* hid_resource=0: neither (the default is on, for bluetoothd's keyboards and mice) */
__startup_func
static void
hidres_init(void)
{
	uint32_t on = 1;
	uint64_t deadline;

	PE_parse_boot_argn("hid_resource", &on, sizeof(on));
	if (on == 0) {
		return;
	}
	start_call = thread_call_allocate(hidres_start, NULL);
	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(start_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hidres_init);
