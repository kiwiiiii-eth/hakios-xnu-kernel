/*
 * libhakios.cc.dylib: inside CommCenter (etc/inject.conf), with boot-arg abb=1 (a test): the Apple Baseband Manager told
 * that the modem is booted, since it is -- it runs on the SoC from the start, and speaks QMI already.
 *
 * ABM (libBasebandManager, inside CommCenter) boots an iPhone's modem itself: BootControllerLegacy::prepare() looks for
 * the modem's firmware to send it (BBUpdater), and without one says "Baseband Firmware Path Not Found", declares the
 * baseband dead and never tries again (r302). Here prepare() and bootModem() answer "done" instead: the two slots of
 * BootControllerLegacy's vtable, in the shared cache's __DATA_CONST, written over (arm64, no pointer authentication). The
 * addresses are those of iOS 16.5 20F66's cache (iPhone10,3/10,6), checked before anything is written: with another
 * cache nothing matches and nothing is changed.
 */
#include <Block.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);   /* (dyld_priv.h) */

#define CACHE_BASE      0x180000000UL   /* where the cache was built to be, before the slide */
#define VTABLE          0x1d90f03c8UL   /* __ZTV20BootControllerLegacy */
#define PREPARE         0x1b7ecd2d0UL   /* BootControllerLegacy::prepare(std::string &) */
#define BOOT_MODEM      0x1b7ecd400UL   /* BootControllerLegacy::bootModem(std::string &) */
#define SLOTS           24
/* BasebandTransport's (and BasebandTransportMAV's, which keeps them) vtables, and its virtual methods taken over */
#define VT_TRANSPORT    0x1d90ef290UL   /* __ZTV17BasebandTransport */
#define VT_MAV          0x1d90f9f70UL   /* __ZTV20BasebandTransportMAV */
#define T_OPEN          0x1b7ebbc8cUL   /* open(Parameters, dispatch::block<void (unsigned char *, unsigned)>) */
#define T_WRITE         0x1b7ebd524UL   /* write(const unsigned char *, unsigned long, unsigned *, bool, unsigned) */
#define T_READ          0x1b7ebda60UL   /* read(unsigned char *, unsigned long, unsigned *, bool, unsigned) */
#define T_CLOSE_SYNC    0x1b7ebe118UL   /* close_sync() */
/* QMITransportService (ABM's QMI transport): its init() watched for the service's own self, its engage() then called once
 * the modem is said booted -- what an iPhone's boot (BBUpdater, the modem's ping) would lead to, and here nothing did:
 * the IPC stayed kDisengaged, no QMux opened (r315) */
#define VT_QMI_SERVICE  0x1d90faca8UL   /* __ZTV19QMITransportService */
#define QS_INIT         0x1b7febb54UL   /* QMITransportService::init() */
#define QS_ENGAGE       0x1b7fec5e8UL   /* QMITransportService::engage() */
#define QS_DUMP         0x1b7fedd48UL   /* dumpState() */
#define QS_SNAPSHOT     0x1b7fedf44UL   /* snapshot(std::string) */
#define QS_WAKE         0x1b7fee5f0UL   /* registerForWakeReason(dispatch::callback<...>) */
#define QS_TEARDOWN     0x1b7fef2f8UL   /* shouldTeardownOnWillReset() */


static uintptr_t the_slide;
static void *volatile qmi_service;

/* a virtual method of the service's watched for its self, then the method itself, its arguments passed on untouched */
#define WATCH(name, address)                                                                                    \
	static uintptr_t                                                                                        \
	name(void *self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g)   \
	{                                                                                                       \
		if (qmi_service == NULL) {                                                                      \
			os_log(OS_LOG_DEFAULT, "hakios: cc: QMITransportService %p (" #name ")", self);          \
		}                                                                                               \
		qmi_service = self;                                                                             \
		return ((uintptr_t (*)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,      \
		    uintptr_t))((address) + the_slide))(self, a, b, c, d, e, f, g);                             \
	}

WATCH(qmi_service_init, QS_INIT)
WATCH(qmi_service_dump, QS_DUMP)
WATCH(qmi_service_snapshot, QS_SNAPSHOT)
WATCH(qmi_service_wake, QS_WAKE)
WATCH(qmi_service_teardown, QS_TEARDOWN)


/*
 * The service's object found among the process's allocations: the one whose first word, or a word a little in (a
 * make_shared's control block is before it), is the service's vtable. Every zone locked while it is looked for, and
 * nothing allocated meanwhile.
 */
static uintptr_t wanted_vtable;
static void *found_object;

static void
look_at(task_t task, void *context, unsigned type, vm_range_t *ranges, unsigned count)
{
	(void)task;
	(void)context;
	(void)type;
	for (unsigned i = 0; i < count && found_object == NULL; i++) {
		uintptr_t *words = (uintptr_t *)ranges[i].address;
		unsigned n = ranges[i].size >= 0x40 ? 8 : (unsigned)(ranges[i].size / 8);

		for (unsigned w = 0; w < n; w++) {
			if (words[w] == wanted_vtable) {
				found_object = &words[w];
				break;
			}
		}
	}
}

static kern_return_t
read_here(task_t task, vm_address_t address, vm_size_t size, void **here)
{
	(void)task;
	(void)size;
	*here = (void *)address;
	return KERN_SUCCESS;
}

static void *
object_of_vtable(uintptr_t vtable)
{
	vm_address_t *zones = NULL;
	unsigned count = 0;

	if (malloc_get_all_zones(mach_task_self(), NULL, &zones, &count) != KERN_SUCCESS) {
		return NULL;
	}
	wanted_vtable = vtable;
	found_object = NULL;
	for (unsigned z = 0; z < count && found_object == NULL; z++) {
		malloc_zone_t *zone = (malloc_zone_t *)zones[z];

		if (zone == NULL || zone->introspect == NULL || zone->introspect->enumerator == NULL) {
			continue;
		}
		zone->introspect->force_lock(zone);
		zone->introspect->enumerator(mach_task_self(), NULL, MALLOC_PTR_IN_USE_RANGE_TYPE, (vm_address_t)zone, read_here, look_at);
		zone->introspect->force_unlock(zone);
	}
	return found_object;
}

/* the modem said booted: its QMux engaged a moment later, as its boot would have had it */
static void
engage_soon(void)
{
	static int tries;

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
		void *service = qmi_service;

		if (service == NULL) {
			service = object_of_vtable(VT_QMI_SERVICE + 0x10 + the_slide);
			os_log(OS_LOG_DEFAULT, "hakios: cc: QMITransportService found among the allocations: %p", service);
		}
		if (service == NULL && ++tries < 10) {
			engage_soon();
			return;
		}
		os_log(OS_LOG_DEFAULT, "hakios: cc: engaging QMITransportService %p", service);
		if (service != NULL) {
			((void (*)(void *))(QS_ENGAGE + the_slide))(service);
		}
	});
}

static bool
prepared(void *self, void *reason)
{
	(void)self;
	(void)reason;
	os_log(OS_LOG_DEFAULT, "hakios: cc: BootControllerLegacy::prepare: the modem needs no firmware from us (it runs already)");
	return true;
}

static bool
booted(void *self, void *reason)
{
	(void)self;
	(void)reason;
	os_log(OS_LOG_DEFAULT, "hakios: cc: BootControllerLegacy::bootModem: the modem is booted already (the SoC's, QMI over QRTR)");
	engage_soon();
	return true;
}

static bool
wanted(void)
{
	char args[2048] = { 0 };
	size_t size = sizeof(args) - 1;
	const char *p;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return false;
	}
	for (p = args; (p = strstr(p, "abb=")) != NULL; p += 4) {
		if ((p == args || p[-1] == ' ') && p[4] >= '1' && p[4] <= '9') {
			return true;
		}
	}
	return false;
}

static bool
write_slot(uintptr_t *slot, uintptr_t value)
{
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

	if (kr != KERN_SUCCESS) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: the vtable's page not made writable: %d", kr);
		return false;
	}
	*slot = value;
	(void)vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
	return true;
}


/*
 * ABM's transport to the modem's QMux channels, over /dev/hakios-qmuxN (the kernel's drivers/modem/hakios.qmux.c, QMUX
 * over this phone's QRTR) instead of an iPhone's PCIe: open() takes a free device and starts a reader that hands each
 * message to the block ABM gave (on a queue of its own, as ABM's reader would from its own); write() writes one message.
 */
typedef void (^qmux_block_t)(unsigned char *, unsigned int);

struct transport {
	void                    *owner;         /* the BasebandTransport */
	int                     fd;
	volatile bool           stop;
	qmux_block_t            block;
	dispatch_queue_t        queue;
	pthread_t               reader;
};

static struct transport transports[4];
static pthread_mutex_t transports_lock = PTHREAD_MUTEX_INITIALIZER;

static struct transport *
transport_of(void *owner)
{
	struct transport *t = NULL;

	pthread_mutex_lock(&transports_lock);
	for (int i = 0; i < 4 && t == NULL; i++) {
		if (transports[i].owner == owner) {
			t = &transports[i];
		}
	}
	pthread_mutex_unlock(&transports_lock);
	return t;
}

static void *
transport_reader(void *argument)
{
	struct transport *t = argument;
	unsigned char *buffer = malloc(16384);
	unsigned long messages = 0;

	while (buffer != NULL && !t->stop) {
		ssize_t n = read(t->fd, buffer, 16384);

		if (n > 0) {
			unsigned char *copy = malloc((size_t)n);
			qmux_block_t block = t->block;

			if (copy == NULL) {
				continue;
			}
			memcpy(copy, buffer, (size_t)n);
			if (messages++ < 6) {
				os_log(OS_LOG_DEFAULT, "hakios: cc: qmux fd %d: %zd bytes from the modem: %02x %02x%02x %02x %02x %02x | %02x",
				    t->fd, n, copy[0], copy[1], copy[2], copy[3], copy[4], copy[5], n > 6 ? copy[6] : 0);
			}
			dispatch_async(t->queue, ^{
				block(copy, (unsigned int)n);
				free(copy);
			});
		} else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
			os_log(OS_LOG_DEFAULT, "hakios: cc: qmux fd %d: read: %d; the reader stops", t->fd, errno);
			break;
		}
	}
	free(buffer);
	return NULL;
}

static bool
transport_open(void *self, void *parameters, void **block)
{
	struct transport *t = transport_of(self);
	char path[32];
	int fd = -1;

	(void)parameters;
	if (t == NULL) {
		pthread_mutex_lock(&transports_lock);
		for (int i = 0; i < 4 && t == NULL; i++) {
			if (transports[i].owner == NULL) {
				t = &transports[i];
				t->owner = self;
				t->fd = -1;
			}
		}
		pthread_mutex_unlock(&transports_lock);
	}
	if (t == NULL) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: transport open: no room for another");
		return false;
	}
	if (t->fd >= 0) {
		return true;
	}
	for (int unit = 0; unit < 4 && fd < 0; unit++) {
		snprintf(path, sizeof(path), "/dev/hakios-qmux%d", unit);
		fd = open(path, O_RDWR);
	}
	if (fd < 0) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: transport open: no /dev/hakios-qmux free: %d", errno);
		return false;
	}
	t->fd = fd;
	t->stop = false;
	t->block = Block_copy((qmux_block_t)(block != NULL ? *block : NULL));
	t->queue = dispatch_queue_create("hakios.qmux", DISPATCH_QUEUE_SERIAL);
	pthread_create(&t->reader, NULL, transport_reader, t);
	os_log(OS_LOG_DEFAULT, "hakios: cc: transport %p open on %{public}s (fd %d)", self, path, fd);
	return true;
}

static bool
transport_write(void *self, const unsigned char *data, unsigned long length, unsigned int *written, bool flag, unsigned int timeout)
{
	struct transport *t = transport_of(self);
	ssize_t n;

	(void)flag;
	(void)timeout;
	if (t == NULL || t->fd < 0) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: transport %p: a write while not open", self);
		return false;
	}
	n = write(t->fd, data, length);
	if (written != NULL) {
		*written = n > 0 ? (unsigned int)n : 0;
	}
	return n == (ssize_t)length;
}

static bool
transport_read(void *self, unsigned char *data, unsigned long length, unsigned int *got, bool flag, unsigned int timeout)
{
	(void)data;
	(void)length;
	(void)flag;
	(void)timeout;
	os_log(OS_LOG_DEFAULT, "hakios: cc: transport %p: read() asked for (the reader hands messages to the block)", self);
	if (got != NULL) {
		*got = 0;
	}
	return false;
}

static bool
transport_close(void *self)
{
	struct transport *t = transport_of(self);

	if (t != NULL && t->fd >= 0) {
		t->stop = true;
		pthread_join(t->reader, NULL);
		close(t->fd);
		t->fd = -1;
		os_log(OS_LOG_DEFAULT, "hakios: cc: transport %p closed", self);
	}
	return true;
}

/* every slot of the vtable at VT (unslid) that holds FROM (unslid) given TO; how many */
static int
take_over(uintptr_t slide, uintptr_t vt, uintptr_t from, void *to)
{
	uintptr_t *slots = (uintptr_t *)(vt + slide);
	int done = 0;

	for (int i = 0; i < SLOTS; i++) {
		if (slots[i] == from + slide && write_slot(&slots[i], (uintptr_t)to)) {
			done++;
		}
	}
	return done;
}

__attribute__((constructor))
static void
hakios_cc_init(void)
{
	size_t length = 0;
	uintptr_t slide;
	uintptr_t *vtable;
	int done = 0;

	if (strcmp(getprogname(), "CommCenter") != 0 || !wanted()) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	/* (the vtable is in the cache, which is mapped whole whether libBasebandManager has been loaded yet or not) */
	if (length == 0) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: no shared cache: nothing changed");
		return;
	}
	vtable = (uintptr_t *)(VTABLE + slide);
	for (int i = 0; i < SLOTS; i++) {
		if (vtable[i] == PREPARE + slide && write_slot(&vtable[i], (uintptr_t)prepared)) {
			done++;
		} else if (vtable[i] == BOOT_MODEM + slide && write_slot(&vtable[i], (uintptr_t)booted)) {
			done++;
		}
	}
	{
		int taken = 0;

		for (int v = 0; v < 2; v++) {
			uintptr_t vt = v == 0 ? VT_TRANSPORT : VT_MAV;

			taken += take_over(slide, vt, T_OPEN, (void *)transport_open);
			taken += take_over(slide, vt, T_WRITE, (void *)transport_write);
			taken += take_over(slide, vt, T_READ, (void *)transport_read);
			taken += take_over(slide, vt, T_CLOSE_SYNC, (void *)transport_close);
		}
		os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 8 of BasebandTransport's slots go to /dev/hakios-qmux", taken);
		the_slide = slide;
		os_log(OS_LOG_DEFAULT, "hakios: cc: %d of QMITransportService's slots watched for its self",
		    take_over(slide, VT_QMI_SERVICE, QS_INIT, (void *)qmi_service_init) +
		    take_over(slide, VT_QMI_SERVICE, QS_DUMP, (void *)qmi_service_dump) +
		    take_over(slide, VT_QMI_SERVICE, QS_SNAPSHOT, (void *)qmi_service_snapshot) +
		    take_over(slide, VT_QMI_SERVICE, QS_WAKE, (void *)qmi_service_wake) +
		    take_over(slide, VT_QMI_SERVICE, QS_TEARDOWN, (void *)qmi_service_teardown));
	}
	os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 2 of BootControllerLegacy's slots answer \"booted\" (slide %#lx; slots 2-5 %#lx %#lx %#lx %#lx)",
	    done, (unsigned long)slide, (unsigned long)(vtable[2] - slide), (unsigned long)(vtable[3] - slide),
	    (unsigned long)(vtable[4] - slide), (unsigned long)(vtable[5] - slide));
}
