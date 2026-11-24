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


/*
 * libPCITransport's kernel side, taken over: acipcKernel is what talks to an iPhone's converged-IPC driver (rings in shared
 * memory, a doorbell); its virtual methods are answered here over /dev/hakios-qmuxN instead, and everything above them --
 * TelephonyBasebandPCITransportCreate, the Mav router's pci_shim, QMux, the QMI clients -- runs as it is. Returns are
 * IOReturns; asynchronous completions call back (context, result, bytes), as IOKit's would.
 */
#define VT_ACIPC        0x20c64c5a0UL   /* __ZTV11acipcKernel */
#define AK_START_RL     0x1e93a9dacUL
#define AK_START_Q      0x1e93a9ebcUL
#define AK_STOP         0x1e93aae8cUL
#define AK_OPEN         0x1e93aa19cUL
#define AK_CLOSE        0x1e93aa1d8UL
#define AK_WRITE        0x1e93aa1e8UL
#define AK_WRITE_ASYNC  0x1e93aa234UL
#define AK_READ         0x1e93aa26cUL
#define AK_READ_ASYNC   0x1e93aa298UL
#define AK_READ_REG     0x1e93aa2d0UL
#define AK_ABORT        0x1e93aa334UL
#define AK_START_CHAN   0x1e93aa45cUL
#define AK_TIMESYNC     0x1e93aa544UL
#define AK_REG_TIME     0x1e93aa5ccUL
#define AK_DEREG_TIME   0x1e93aa630UL
#define AK_MAP          0x1e93aa6a8UL
#define AK_UNMAP        0x1e93aa740UL

#define IORET_OK        0
#define IORET_ABORTED   (int)0xe00002eb
#define IORET_ERROR     (int)0xe00002bc
#define IORET_NOT_OPEN  (int)0xe00002cd

typedef void (*io_done_t)(void *, int, void *);
typedef void (*io_done2_t)(void *, int);

struct link {
	void                    *owner;
	int                     fd;
	unsigned                interface;
	volatile bool           stop;
	pthread_t               reader;
	dispatch_queue_t        queue;
	pthread_mutex_t         lock;
	/* the read waiting for a message */
	void                    *read_buffer;
	unsigned                read_length;
	io_done_t               read_done;
	void                    *read_context;
	/* messages come before a read asked for them */
	unsigned char           *pending[32];
	unsigned                pending_length[32];
	unsigned                pending_head, pending_tail;
	unsigned long           in, out;
};

static struct link links[8];
static pthread_mutex_t links_lock = PTHREAD_MUTEX_INITIALIZER;

static struct link *
link_of(void *owner, bool make)
{
	struct link *l = NULL;

	pthread_mutex_lock(&links_lock);
	for (int i = 0; i < 8 && l == NULL; i++) {
		if (links[i].owner == owner) {
			l = &links[i];
		}
	}
	for (int i = 0; i < 8 && l == NULL && make; i++) {
		if (links[i].owner == NULL) {
			l = &links[i];
			memset(l, 0, sizeof(*l));
			l->owner = owner;
			l->fd = -1;
			pthread_mutex_init(&l->lock, NULL);
		}
	}
	pthread_mutex_unlock(&links_lock);
	return l;
}

static dispatch_queue_t
link_queue(struct link *l)
{
	return l->queue != NULL ? l->queue : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
}

/* with the lock held: a waiting read given the oldest message, if both are there */
static void
link_deliver(struct link *l)
{
	if (l->read_done == NULL || l->pending_head == l->pending_tail) {
		return;
	}
	unsigned slot = l->pending_tail % 32;
	unsigned char *m = l->pending[slot];
	unsigned n = l->pending_length[slot];
	io_done_t done = l->read_done;
	void *context = l->read_context;

	if (n > l->read_length) {
		n = l->read_length;
	}
	memcpy(l->read_buffer, m, n);
	free(m);
	l->pending_tail++;
	l->read_done = NULL;
	l->in++;
	dispatch_async(link_queue(l), ^{
		done(context, IORET_OK, (void *)(uintptr_t)n);
	});
}

static void *
link_reader(void *argument)
{
	struct link *l = argument;
	unsigned char *buffer = malloc(16384);

	while (buffer != NULL && !l->stop) {
		ssize_t n = read(l->fd, buffer, 16384);

		if (n > 0) {
			unsigned char *m = malloc((size_t)n);

			if (m == NULL) {
				continue;
			}
			memcpy(m, buffer, (size_t)n);
			if (l->in < 6) {
				os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %u: %zd bytes from the modem: %02x %02x%02x %02x %02x %02x | %02x %02x",
				    l->interface, n, m[0], m[1], m[2], m[3], m[4], m[5], n > 6 ? m[6] : 0, n > 7 ? m[7] : 0);
			}
			pthread_mutex_lock(&l->lock);
			if (l->pending_head - l->pending_tail < 32) {
				l->pending[l->pending_head % 32] = m;
				l->pending_length[l->pending_head % 32] = (unsigned)n;
				l->pending_head++;
			} else {
				free(m);
			}
			link_deliver(l);
			pthread_mutex_unlock(&l->lock);
		} else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
			break;
		}
	}
	free(buffer);
	return NULL;
}

static int
ak_open(void *self, unsigned interface)
{
	struct link *l = link_of(self, true);
	char path[32];
	int fd = -1;

	if (l == NULL) {
		return IORET_ERROR;
	}
	if (l->fd >= 0) {
		return IORET_OK;
	}
	for (int unit = 0; unit < 4 && fd < 0; unit++) {
		snprintf(path, sizeof(path), "/dev/hakios-qmux%d", unit);
		fd = open(path, O_RDWR);
	}
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: interface %u opened on %{public}s (fd %d)", self, interface, fd >= 0 ? path : "nothing", fd);
	if (fd < 0) {
		return IORET_ERROR;
	}
	l->fd = fd;
	l->interface = interface;
	l->stop = false;
	pthread_create(&l->reader, NULL, link_reader, l);
	return IORET_OK;
}

static int
ak_close(void *self)
{
	struct link *l = link_of(self, false);

	if (l != NULL && l->fd >= 0) {
		l->stop = true;
		pthread_join(l->reader, NULL);
		close(l->fd);
		l->fd = -1;
		os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: closed (%lu in, %lu out)", self, l->in, l->out);
	}
	return IORET_OK;
}

static int
ak_start_queue(void *self, unsigned interface, dispatch_queue_t queue, void *notify, void *other)
{
	struct link *l = link_of(self, true);

	(void)notify;
	(void)other;
	if (l != NULL) {
		l->queue = queue;
		l->interface = interface;
	}
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: started for interface %u", self, interface);
	return ak_open(self, interface);
}

static int
ak_start_runloop(void *self, unsigned interface, void *runloop, void *notify, void *other)
{
	(void)runloop;
	return ak_start_queue(self, interface, NULL, notify, other);
}

static int
ak_stop(void *self, void *group)
{
	(void)group;
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: stopped", self);
	return IORET_OK;
}

static int
ak_write(void *self, const void *data, unsigned length)
{
	struct link *l = link_of(self, false);

	if (l == NULL || l->fd < 0) {
		return IORET_NOT_OPEN;
	}
	l->out++;
	return write(l->fd, data, length) == (ssize_t)length ? IORET_OK : IORET_ERROR;
}

static int
ak_write_async(void *self, const void *data, unsigned length, io_done_t done, void *context)
{
	struct link *l = link_of(self, false);
	int result;

	if (l == NULL || l->fd < 0) {
		return IORET_NOT_OPEN;
	}
	if (l->out < 6) {
		const unsigned char *b = data;

		os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %u: %u bytes to the modem: %02x %02x%02x %02x %02x %02x | %02x %02x",
		    l->interface, length, b[0], length > 1 ? b[1] : 0, length > 2 ? b[2] : 0, length > 3 ? b[3] : 0,
		    length > 4 ? b[4] : 0, length > 5 ? b[5] : 0, length > 6 ? b[6] : 0, length > 7 ? b[7] : 0);
	}
	l->out++;
	result = write(l->fd, data, length) == (ssize_t)length ? IORET_OK : IORET_ERROR;
	if (done != NULL) {
		dispatch_async(link_queue(l), ^{
			done(context, result, (void *)(uintptr_t)(result == IORET_OK ? length : 0));
		});
	}
	return IORET_OK;
}

static int
ak_read(void *self, void *data, unsigned *length)
{
	(void)self;
	(void)data;
	if (length != NULL) {
		*length = 0;
	}
	return IORET_ERROR;                     /* (asynchronous reads only) */
}

static int
ak_read_async(void *self, void *data, unsigned length, io_done_t done, void *context)
{
	struct link *l = link_of(self, false);

	if (l == NULL || l->fd < 0) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: a read while not open", self);
		return IORET_NOT_OPEN;
	}
	if (l->in < 3) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %u: a read of up to %u bytes waits", l->interface, length);
	}
	pthread_mutex_lock(&l->lock);
	l->read_buffer = data;
	l->read_length = length;
	l->read_done = done;
	l->read_context = context;
	link_deliver(l);
	pthread_mutex_unlock(&l->lock);
	return IORET_OK;
}

static int
ak_abort(void *self, int direction, io_done2_t done, void *context)
{
	struct link *l = link_of(self, false);
	io_done_t read_done = NULL;
	void *read_context = NULL;

	(void)direction;
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: channel %d aborted", self, direction);
	if (l != NULL) {
		pthread_mutex_lock(&l->lock);
		read_done = l->read_done;
		read_context = l->read_context;
		l->read_done = NULL;
		pthread_mutex_unlock(&l->lock);
	}
	dispatch_async(l != NULL ? link_queue(l) : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
		if (read_done != NULL) {
			read_done(read_context, IORET_ABORTED, NULL);
		}
		if (done != NULL) {
			done(context, IORET_OK);
		}
	});
	return IORET_OK;
}

static int
ak_start_channel(void *self, int direction, io_done2_t done, void *context)
{
	struct link *l = link_of(self, false);

	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: channel %d started", self, direction);
	(void)direction;
	if (done != NULL) {
		dispatch_async(l != NULL ? link_queue(l) : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
			done(context, IORET_OK);
		});
	}
	return IORET_OK;
}

static int
ak_map(void *self, int region, void *a, unsigned b, void *c, unsigned d, io_done_t done, void *context)
{
	struct link *l = link_of(self, false);

	(void)a;
	(void)c;
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: region %d mapped (%u, %u bytes)", self, region, b, d);
	if (done != NULL) {
		dispatch_async(l != NULL ? link_queue(l) : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
			done(context, IORET_OK, NULL);
		});
	}
	return IORET_OK;
}

static int
ak_unmap(void *self, int region, io_done2_t done, void *context)
{
	(void)region;
	return ak_start_channel(self, 0, done, context);
}

static int
ak_unsupported(void *self)
{
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: something unsupported asked for", self);
	return IORET_ERROR;
}

static int
ak_nothing(void *self)
{
	(void)self;
	return IORET_OK;
}


/* ... and libPCITransport's control side (acipcKernelControl: the converged IPC's control client), answered too */
#define VT_ACIPC_CTL    0x20c64c980UL   /* __ZTV18acipcKernelControl */
#define KC_START        0x1e93aa8b8UL
#define KC_STOP         0x1e93aaf1cUL
#define KC_OPEN_RL      0x1e93aaf24UL
#define KC_OPEN_Q       0x1e93aaf2cUL
#define KC_CLOSE        0x1e93aaf34UL
#define KC_READ_LOGS    0x1e93aaf3cUL
#define KC_EVENTS_RL    0x1e93aa930UL
#define KC_EVENTS_Q     0x1e93aa9b4UL
#define KC_NO_EVENTS    0x1e93aaf44UL
#define KC_LOG_SIZE     0x1e93aaf4cUL
#define KC_LOG_TUNE     0x1e93aaf54UL
#define KC_BOOT_STAGE   0x1e93aaf5cUL

static void *control_events;            /* the block ABM gave for the control's events */

static int
kc_ok(void *self)
{
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc control %p: done", self);
	return IORET_OK;
}

static int
kc_open(void *self, void *queue)
{
	(void)queue;
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc control %p: opened", self);
	return IORET_OK;
}

static int
kc_events(void *self, void *queue, void *block)
{
	(void)queue;
	control_events = block != NULL ? Block_copy(block) : NULL;
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc control %p: events will go to %p", self, block);
	return IORET_OK;
}

static int
kc_boot_stage(void *self, unsigned *stage)
{
	(void)self;
	if (stage != NULL) {
		*stage = 0;
	}
	os_log(OS_LOG_DEFAULT, "hakios: cc: acipc control: boot stage asked for");
	return IORET_OK;
}

static int
kc_unsupported(void *self)
{
	(void)self;
	return IORET_ERROR;
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
	/* CommCenter's standard error and output to a file: libPCITransport says what fails there, not in the log */
	{
		int fd = open("/private/var/tmp/cc-stderr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd >= 0) {
			dup2(fd, 2);
			dup2(fd, 1);
			close(fd);
			setvbuf(stderr, NULL, _IONBF, 0);
			setvbuf(stdout, NULL, _IOLBF, 0);
		}
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
	{
		int taken = 0;

		taken += take_over(slide, VT_ACIPC, AK_START_RL, (void *)ak_start_runloop);
		taken += take_over(slide, VT_ACIPC, AK_START_Q, (void *)ak_start_queue);
		taken += take_over(slide, VT_ACIPC, AK_STOP, (void *)ak_stop);
		taken += take_over(slide, VT_ACIPC, AK_OPEN, (void *)ak_open);
		taken += take_over(slide, VT_ACIPC, AK_CLOSE, (void *)ak_close);
		taken += take_over(slide, VT_ACIPC, AK_WRITE, (void *)ak_write);
		taken += take_over(slide, VT_ACIPC, AK_WRITE_ASYNC, (void *)ak_write_async);
		taken += take_over(slide, VT_ACIPC, AK_READ, (void *)ak_read);
		taken += take_over(slide, VT_ACIPC, AK_READ_ASYNC, (void *)ak_read_async);
		taken += take_over(slide, VT_ACIPC, AK_READ_REG, (void *)ak_unsupported);
		taken += take_over(slide, VT_ACIPC, AK_ABORT, (void *)ak_abort);
		taken += take_over(slide, VT_ACIPC, AK_START_CHAN, (void *)ak_start_channel);
		taken += take_over(slide, VT_ACIPC, AK_TIMESYNC, (void *)ak_unsupported);
		taken += take_over(slide, VT_ACIPC, AK_REG_TIME, (void *)ak_nothing);
		taken += take_over(slide, VT_ACIPC, AK_DEREG_TIME, (void *)ak_nothing);
		taken += take_over(slide, VT_ACIPC, AK_MAP, (void *)ak_map);
		taken += take_over(slide, VT_ACIPC, AK_UNMAP, (void *)ak_unmap);
		os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 17 of acipcKernel's slots go to /dev/hakios-qmux", taken);
	}
	{
		int taken = 0;

		taken += take_over(slide, VT_ACIPC_CTL, KC_START, (void *)kc_ok);
		taken += take_over(slide, VT_ACIPC_CTL, KC_STOP, (void *)kc_ok);
		taken += take_over(slide, VT_ACIPC_CTL, KC_OPEN_RL, (void *)kc_open);
		taken += take_over(slide, VT_ACIPC_CTL, KC_OPEN_Q, (void *)kc_open);
		taken += take_over(slide, VT_ACIPC_CTL, KC_CLOSE, (void *)kc_ok);
		taken += take_over(slide, VT_ACIPC_CTL, KC_READ_LOGS, (void *)kc_unsupported);
		taken += take_over(slide, VT_ACIPC_CTL, KC_EVENTS_RL, (void *)kc_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_EVENTS_Q, (void *)kc_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_NO_EVENTS, (void *)kc_ok);
		taken += take_over(slide, VT_ACIPC_CTL, KC_LOG_SIZE, (void *)kc_unsupported);
		taken += take_over(slide, VT_ACIPC_CTL, KC_LOG_TUNE, (void *)kc_ok);
		taken += take_over(slide, VT_ACIPC_CTL, KC_BOOT_STAGE, (void *)kc_boot_stage);
		os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 12 of acipcKernelControl's slots answered here", taken);
	}
	os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 2 of BootControllerLegacy's slots answer \"booted\" (slide %#lx; slots 2-5 %#lx %#lx %#lx %#lx)",
	    done, (unsigned long)slide, (unsigned long)(vtable[2] - slide), (unsigned long)(vtable[3] - slide),
	    (unsigned long)(vtable[4] - slide), (unsigned long)(vtable[5] - slide));
}
