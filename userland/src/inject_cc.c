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
#include "inject_cc_uim.h"
#include "inject_cc_qmux.h"
#include <stdatomic.h>
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
#include <libkern/OSCacheControl.h>
#include <sys/sysctl.h>
#include <sys/stat.h>

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
	int abb = 0;
	size_t abb_size = sizeof(abb);

	/* (kern.hakios.abb: the kernel's view, the kernel's own arguments included; a kernel without it: the boot line) */
	if (sysctlbyname("kern.hakios.abb", &abb, &abb_size, NULL, 0) == 0) {
		return abb != 0;
	}
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
#define AK_DTOR         0x1e93aae6cUL
#define AK_DELETE       0x1e93aae70UL
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

#define LINK_PENDING_SLOTS 256

struct link {
	void                    *owner;
	int                     fd;
	unsigned                interface;
	atomic_bool             stop;
	pthread_t               reader;
	dispatch_queue_t        queue;
	pthread_mutex_t         lock;
	/* the read waiting for a message */
	void                    *read_buffer;
	unsigned                read_length;
	io_done_t               read_done;
	void                    *read_context;
	/* messages come before a read asked for them */
	unsigned char           *pending[LINK_PENDING_SLOTS];
	unsigned                pending_length[LINK_PENDING_SLOTS];
	unsigned                pending_head, pending_tail, pending_highwater, pending_dropped;
	unsigned long           in, out;
    unsigned seen_tx[256], seen_rx[256];
    atomic_bool uim_slot_swapped;
    unsigned uim_metadata_count;
    struct { unsigned txid,client,file,session; bool valid; } file_reads[128];
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
            atomic_init(&l->uim_slot_swapped,false);
            atomic_init(&l->stop,false);
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
	unsigned slot = l->pending_tail % LINK_PENDING_SLOTS;
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

/* Native UMTSProvisioningSlotInfo stores 15 actual identity digits in
 * high-nibble-first BCD, with final F padding (native helper verified). The
 * real UIM EF has length/parity and low-nibble-first encoding. Keep only this
 * transient in-process native representation, never emit its contents.
 * Unknown/malformed/unsupported lengths remain unknown; no synthetic digits.
 */
static pthread_mutex_t sim_info_lock=PTHREAD_MUTEX_INITIALIZER;
static bool real_sim_info_valid;
static bool real_card_info_valid;
static unsigned char real_card_info_bcd[10];
static unsigned char real_sim_info_bcd[8];
static bool normalize_real_sim_info(const unsigned char *data,size_t n,unsigned char out[8])
{
    if(!data || !out || n<9 || data[0]!=8 || (data[1]&15)!=9)return false;
    unsigned char converted[8];memset(converted,0xff,sizeof(converted));
    for(unsigned digit=0;digit<15;digit++) {
        unsigned nibble=digit+1,byte=1+nibble/2;
        unsigned value=(nibble&1)?data[byte]>>4:data[byte]&15;
        if(value>9)return false;
        if((digit&1)==0)converted[digit/2]=(unsigned char)((value<<4)|15);
        else converted[digit/2]=(unsigned char)((converted[digit/2]&0xf0)|value);
    }
    memcpy(out,converted,sizeof(converted));return true;
}
/* EF ICCID has ten low-first BCD bytes, with at most a final F pad.
 * The native helper expects high-first BCD. Validate the real 89 prefix and
 * every digit; preserve a genuine final padding nibble, never invent digits.
 */
static bool normalize_real_card_info(const unsigned char *data,size_t n,unsigned char out[10])
{
    if(!data || !out || n!=10)return false;
    unsigned char converted[10];
    for(unsigned i=0;i<20;i++) {
        unsigned value=(i&1)?data[i/2]>>4:data[i/2]&15;
        if(value>9 && !(i==19 && value==15))return false;
        if(!(i&1))converted[i/2]=(unsigned char)(value<<4);
        else converted[i/2]|=(unsigned char)value;
    }
    if(converted[0]!=0x89)return false;
    memcpy(out,converted,sizeof(converted));return true;
}
static bool supply_real_sim_info(unsigned char *info)
{
    bool supplied=false,slot_known=info[0x1d8] && info[0x120];
    pthread_mutex_lock(&sim_info_lock);
    bool subscriber_known=(slot_known && info[0x80]) || real_sim_info_valid;
    bool card_known=(slot_known && info[0x8b]) || real_card_info_valid;
    /* First publish a native slot only when both required matching fields
     * are genuinely available. Preserve existing native fields unchanged. */
    if(!subscriber_known || !card_known){pthread_mutex_unlock(&sim_info_lock);return false;}
    if(real_sim_info_valid && !(slot_known && info[0x80])) {
        memcpy(info+0x78,real_sim_info_bcd,8);info[0x80]=1;supplied=true;
    }
    if(real_card_info_valid && !(slot_known && info[0x8b])) {
        memcpy(info+0x81,real_card_info_bcd,10);info[0x8b]=1;supplied=true;
    }
    if(supplied){info[0x120]=1;info[0x1d8]=1;}
    pthread_mutex_unlock(&sim_info_lock);
    return supplied;
}
/* EF IMSI: length byte, parity nibble, then BCD identity. Extract only
 * the first three country digits, following the format used by ModemManager's
 * uim_get_imsi_ready. Never retain or log the remaining identity digits. */
static unsigned sim_country_mcc(const unsigned char *data,size_t n)
{
    if(!data || n<3 || data[0]<2 || data[0]>8 || (size_t)data[0]+1>n)return 0;
    unsigned parity=data[1]&15,a=data[1]>>4,b=data[2]&15,c=data[2]>>4;
    if((parity!=1 && parity!=9) || a>9 || b>9 || c>9)return 0;
    unsigned mcc=a*100+b*10+c;
    return mcc>=100?mcc:0;
}
/* Read-only UIM file transaction metadata. No file contents or identifiers. */
static void uim_file_metadata(struct link *l,const unsigned char *b,size_t n,bool receive)
{
    if(n<13 || b[0]!=1 || b[4]!=0x0b || (b[9]|b[10]<<8)!=0x20 || b[6]!=(receive?2:0))return;
    unsigned txid=b[7]|b[8]<<8,file=0,session=255,bytes=0;
    int result=-1,error=-1;
    const unsigned char *read_data=NULL;size_t read_length=0;
    for(size_t off=13;off+3<=n;) {
        size_t len=b[off+1]|b[off+2]<<8;if(len>n-off-3)return;
        const unsigned char *v=b+off+3;
        if(!receive && b[off]==2 && len>=3)file=v[0]|v[1]<<8;
        if(!receive && b[off]==1 && len>=2)session=v[0];
        if(receive && b[off]==2 && len==4){result=v[0]|v[1]<<8;error=v[2]|v[3]<<8;}
        if(receive && b[off]==0x11 && len>=2){unsigned want=v[0]|v[1]<<8;if(want<=len-2){bytes=want;read_data=v+2;read_length=want;}}
        off+=3+len;
    }
    unsigned slot=txid%128;
    pthread_mutex_lock(&l->lock);
    if(!receive && file) {
        l->file_reads[slot].txid=txid;l->file_reads[slot].client=b[5];
        l->file_reads[slot].file=file;l->file_reads[slot].session=session;l->file_reads[slot].valid=true;
    } else if(receive && l->file_reads[slot].valid && l->file_reads[slot].txid==txid && l->file_reads[slot].client==b[5]) {
        file=l->file_reads[slot].file;session=l->file_reads[slot].session;l->file_reads[slot].valid=false;
    }
    pthread_mutex_unlock(&l->lock);
    if(receive && file==0x2fe2 && result==0) {
        unsigned char normalized[10];
        bool valid=normalize_real_card_info(read_data,read_length,normalized);
        pthread_mutex_lock(&sim_info_lock);real_card_info_valid=valid;
        if(valid)memcpy(real_card_info_bcd,normalized,10);
        else memset(real_card_info_bcd,0,10);
        pthread_mutex_unlock(&sim_info_lock);memset(normalized,0,10);
        os_log(OS_LOG_DEFAULT,"hakios: cc: sim-info-v2 real card-format supported=%d",valid);
    }
    if(receive && file==0x6f07 && result==0) {
        unsigned mcc=sim_country_mcc(read_data,read_length);
        unsigned char normalized[8];
        bool valid=normalize_real_sim_info(read_data,read_length,normalized);
        pthread_mutex_lock(&sim_info_lock);
        real_sim_info_valid=valid;
        if(valid)memcpy(real_sim_info_bcd,normalized,sizeof(normalized));
        else memset(real_sim_info_bcd,0,sizeof(real_sim_info_bcd));
        pthread_mutex_unlock(&sim_info_lock);
        memset(normalized,0,sizeof(normalized));
        os_log(OS_LOG_DEFAULT,"hakios: cc: sim-info-v1 real UIM conversion supported=%d",valid);

        int serving=0;size_t size=sizeof(serving);
        bool known=sysctlbyname("kern.hakios.cellular_mcc",&serving,&size,NULL,0)==0;
        os_log(OS_LOG_DEFAULT,"hakios: cc-uim: sim-country-v1 sim-mcc=%u serving-known=%d serving-mcc=%d",mcc,known,serving);
    }
    if(file)os_log(OS_LOG_DEFAULT,"hakios: cc-uim: file-read %{public}s file=%#x session=%u result=%d error=%d data-length=%u",receive?"rx":"tx",file,session,result,error,bytes);
}

/* Header/result metadata only; never print a QMI payload or subscriber identity. */
static void
qmi_metadata(struct link *l, const unsigned char *b, size_t n, bool receive)
{
    uim_file_metadata(l,b,n,receive);
    if (n < 13 || b[0] != 1 || b[4] == 0) return;
    if(b[4]==1 && b[6]==0 && (b[9] | b[10]<<8)==0xa2) {
        for(size_t off=13;off+3<=n;) {
            size_t len=b[off+1] | b[off+2]<<8;
            if(len>n-off-3) break;
            const uint8_t *v=b+off+3;
            os_log(OS_LOG_DEFAULT,"hakios: cc-wds: bind client=%u tlv=%#x length=%zu",b[5],b[off],len);
            if(b[off]==0x11 && len==1) os_log(OS_LOG_DEFAULT,"hakios: cc-wds: bind client=%u mux=%u",b[5],v[0]);
            if(b[off]==0x10 && len==8) os_log(OS_LOG_DEFAULT,"hakios: cc-wds: bind client=%u endpoint=%u interface=%u",b[5],(unsigned)(v[0]|v[1]<<8|v[2]<<16|v[3]<<24),(unsigned)(v[4]|v[5]<<8|v[6]<<16|v[7]<<24));
            off+=3+len;
        }
    }

    unsigned service=b[4], message=b[9] | b[10]<<8;
    unsigned *seen=receive ? l->seen_rx : l->seen_tx;
    if (seen[service]++ >= 100) return;
    int result=-1, error=-1;
    for (size_t off=13; off+3<=n;) {
        size_t length=b[off+1] | b[off+2]<<8;
        if (off+3+length>n) break;
        if (b[off]==2 && length==4) { result=b[off+3] | b[off+4]<<8; error=b[off+5] | b[off+6]<<8; break; }
        off+=3+length;
    }
    os_log(OS_LOG_DEFAULT, "hakios: cc-qmi: %{public}s service=%#x message=%#x bytes=%zu flags=%u result=%d error=%d",
        receive ? "rx" : "tx", service, message, n, b[6], result, error);
}

/* Translate only the two real cards and known explicit slot selectors. No
 * card state, application, subscriber data or result code is synthesized. */
static void
uim_receive(struct link *l, unsigned char *b, size_t n)
{
    if(n<13 || b[0]!=1 || b[4]!=0x0b) return;
    unsigned msg=b[9] | b[10]<<8;
    if(!((b[6]==2 && msg==0x2f) || (b[6]==4 && msg==0x32))) return;

    for(size_t off=13;off+3<=n;) {
        size_t len=b[off+1] | b[off+2]<<8;
        if(len>n-off-3) return;
        unsigned char *v=b+off+3;
        if(l->uim_metadata_count++<30)
            os_log(OS_LOG_DEFAULT,"hakios: cc-uim: rx message=%#x tlv=%#x length=%zu",msg,b[off],len);
        if(b[off]==0x10 && len>=9) {
            unsigned before=v[1],cards=v[8];
            size_t second=0;bool any_card=len>9 && v[9]==1;
            if(cards>=2 && uim_card_end(v,len,9,&second) && second<len)any_card=any_card || v[second]==1;
            if(!any_card) {
                pthread_mutex_lock(&sim_info_lock);real_sim_info_valid=false;real_card_info_valid=false;
                memset(real_sim_info_bcd,0,sizeof(real_sim_info_bcd));memset(real_card_info_bcd,0,sizeof(real_card_info_bcd));pthread_mutex_unlock(&sim_info_lock);
            }

            size_t end=0;
            if(len>=15 && uim_card_end(v,len,9,&end) && end<len)
                os_log(OS_LOG_DEFAULT,"hakios: cc-uim: original card states=%u,%u first-usim-state=%u",v[9],v[end],end+7<len ? v[end+7] : 255);
            int mapped=uim_swap_real_card(v,len);
            if(mapped) atomic_store(&l->uim_slot_swapped,true);
            os_log(OS_LOG_DEFAULT,"hakios: cc-uim: cards=%u mapped=%d primary-slot-before=%u after=%u",cards,mapped,before,v[1]);
        }
        off+=3+len;
    }
}

static ssize_t
link_write_data(struct link *l, const void *data, unsigned n)
{
    const unsigned char *b=data;
    size_t mapped_size=n;
    uint8_t *endpoint=qmux_bind_ipa_endpoint(b,n,&mapped_size);
    if(endpoint) {
        os_log(OS_LOG_DEFAULT,"hakios: cc-wds: supplied actual IPA endpoint=4 interface=1 client=%u; original mux retained",b[5]);
        ssize_t written=write(l->fd,endpoint,mapped_size);free(endpoint);
        return written==(ssize_t)mapped_size ? (ssize_t)n : -1;
    }
    if(n<13 || b[0]!=1 || b[4]!=0x0b || b[6]!=0 || !atomic_load(&l->uim_slot_swapped))
        return write(l->fd,data,n);
    unsigned msg=b[9] | b[10]<<8;
    unsigned char *copy=malloc(n);
    if(!copy){errno=ENOMEM;return -1;}
    memcpy(copy,b,n);
    unsigned changed=0;
    for(size_t off=13;off+3<=n;) {
        size_t len=copy[off+1] | copy[off+2]<<8;
        if(len>n-off-3){free(copy);errno=EINVAL;return -1;}
        unsigned char *v=copy+off+3;
        if((msg==0x30 || msg==0x31) && copy[off]==1 && len==1 && (v[0]==1 || v[0]==2)) {
            v[0]=3-v[0];changed++;
        } else if(((msg==0x38 && copy[off]==0x10) || (msg==0x39 && copy[off]==1)) &&
                  len>=2 && len==2u+v[1] && (v[0]==1 || v[0]==2)) {
            v[0]=3-v[0];changed++;
        } else if((msg==0x20 || msg==0x21 || msg==0x24) && copy[off]==1 && len>=2 && len==2u+v[1] && v[0]>=4 && v[0]<=9) {
            v[0]^=1;changed++;
        }
        off+=3+len;
    }
    if(changed) os_log(OS_LOG_DEFAULT,"hakios: cc-uim: tx message=%#x selectors-mapped=%u",msg,changed);
    ssize_t result=write(l->fd,copy,n);
    free(copy);return result;
}

static void *
link_reader(void *argument)
{
	struct link *l = argument;
	unsigned char *buffer = malloc(16384);

	while (buffer != NULL && !atomic_load(&l->stop)) {
		ssize_t n = read(l->fd, buffer, 16384);

		if (n > 0) {
			unsigned char *m = malloc((size_t)n);

			if (m == NULL) {
				continue;
			}
			memcpy(m, buffer, (size_t)n);
            uim_receive(l,m,(size_t)n);
            qmi_metadata(l,m,(size_t)n,true);
			if (l->in < 6) {
				os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %u: %zd bytes from the modem: %02x %02x%02x %02x %02x %02x | %02x %02x",
				    l->interface, n, m[0], m[1], m[2], m[3], m[4], m[5], n > 6 ? m[6] : 0, n > 7 ? m[7] : 0);
			}
			pthread_mutex_lock(&l->lock);
			if (l->pending_head - l->pending_tail < LINK_PENDING_SLOTS) {
				l->pending[l->pending_head % LINK_PENDING_SLOTS] = m;
				l->pending_length[l->pending_head % LINK_PENDING_SLOTS] = (unsigned)n;
				l->pending_head++;
                unsigned pending=l->pending_head-l->pending_tail;
                if(pending>l->pending_highwater)l->pending_highwater=pending;
                if(pending==32 || pending==64 || pending==128 || pending==LINK_PENDING_SLOTS)
                    os_log(OS_LOG_DEFAULT,"hakios: cc: queue-v2 interface=%u pending=%u capacity=%u dropped=%u",l->interface,pending,LINK_PENDING_SLOTS,l->pending_dropped);
			} else {
                l->pending_dropped++;
                if(l->pending_dropped==1 || (l->pending_dropped&63)==0)
                    os_log(OS_LOG_DEFAULT,"hakios: cc: queue-v2 dropped=%u interface=%u",l->pending_dropped,l->interface);
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
	atomic_store(&l->stop,false);
	pthread_create(&l->reader, NULL, link_reader, l);
	return IORET_OK;
}

static int
ak_close(void *self)
{
	struct link *l = link_of(self, false);

	if (l != NULL && l->fd >= 0) {
		atomic_store(&l->stop,true);
		pthread_join(l->reader, NULL);
		close(l->fd);
		l->fd = -1;
		os_log(OS_LOG_DEFAULT, "hakios: cc: acipc %p: closed (%lu in, %lu out)", self, l->in, l->out);
	}
	return IORET_OK;
}

/* Native teardown destroys acipcKernel without calling its virtual close.
 * Its native destructor only knows Apple's embedded driver, not our fd/thread.
 * Close/join before native destruction, discard unconsumed packets, and release
 * the bridge slot. Completion blocks already submitted capture their values,
 * not this slot. The reader is gone before the owner/lock can be reused.
 */
static void ak_dispose_link(void *self)
{
    struct link *l=link_of(self,false);
    if(!l)return;
    (void)ak_close(self);
    pthread_mutex_lock(&l->lock);
    unsigned discarded=l->pending_head-l->pending_tail;
    while(l->pending_tail!=l->pending_head) {
        unsigned slot=l->pending_tail++ % LINK_PENDING_SLOTS;
        free(l->pending[slot]);l->pending[slot]=NULL;
    }
    /* Native teardown cancels the request before destroying this kernel object;
     * never invoke an abandoned callback on an owner being destructed. */
    bool abandoned=l->read_done!=NULL;
    l->read_done=NULL;l->read_buffer=NULL;l->read_context=NULL;l->read_length=0;
    pthread_mutex_unlock(&l->lock);
    os_log(OS_LOG_DEFAULT,"hakios: cc: lifecycle-v2 owner disposed discarded=%u pending-read=%d max-pending=%u dropped=%u",discarded,abandoned,l->pending_highwater,l->pending_dropped);
    pthread_mutex_lock(&links_lock);
    pthread_mutex_destroy(&l->lock);
    memset(l,0,sizeof(*l));l->fd=-1;
    pthread_mutex_unlock(&links_lock);
}
static void ak_destructor(void *self)
{
    ak_dispose_link(self);
    ((void (*)(void *))(AK_DTOR+the_slide))(self);
}
static void ak_deleting_destructor(void *self)
{
    ak_dispose_link(self);
    ((void (*)(void *))(AK_DELETE+the_slide))(self);
}

/* iOS 16.5 acipcKernel::start is BOOL, not IOReturn. The caller also waits
 * for the interface-added block before calling open(service). The two blocks
 * take an unsigned service token; the last block is the error notification.
 * Token 1 represents our already-open local bridge, never an IOKit service. */
typedef void (^ak_service_block_t)(unsigned);
static bool
ak_start_queue(void *self, unsigned interface, dispatch_queue_t queue,
    ak_service_block_t added, ak_service_block_t removed, void *notification,
    unsigned options, int timeout)
{
    struct link *l = link_of(self, true);
    (void)removed; (void)notification; (void)options; (void)timeout;
    if (l == NULL) return false;
    l->queue = queue;
    l->interface = interface;
    int status = ak_open(self, interface);
    os_log(OS_LOG_DEFAULT, "hakios: cc: start-v2: interface %u open-status %#x accepted %d added %d",
        interface, status, status == IORET_OK, added != NULL);
    if (status != IORET_OK) return false;
    if (added == NULL) return false;
    ak_service_block_t callback = Block_copy(added);
    dispatch_async(link_queue(l), ^{
        os_log(OS_LOG_DEFAULT, "hakios: cc: start-v2: interface %u reporting local bridge added", interface);
        callback(1);
        Block_release(callback);
    });
    return true;
}

static bool
ak_start_runloop(void *self, unsigned interface, void *runloop,
    ak_service_block_t added, ak_service_block_t removed, void *notification,
    unsigned options, int timeout)
{
    (void)runloop;
    return ak_start_queue(self, interface, NULL, added, removed, notification, options, timeout);
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
	qmi_metadata(l,data,length,false);
	l->out++;
	return link_write_data(l, data, length) == (ssize_t)length ? IORET_OK : IORET_ERROR;
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
	qmi_metadata(l,data,length,false);
	l->out++;
	result = link_write_data(l, data, length) == (ssize_t)length ? IORET_OK : IORET_ERROR;
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

static void *control_owner;
static pthread_mutex_t control_events_lock=PTHREAD_MUTEX_INITIALIZER;
static void *control_events;            /* the block ABM gave for the control's events */

static bool
kc_start(void *self)
{
    os_log(OS_LOG_DEFAULT, "hakios: cc: control-start-v2: accepted");
    (void)self;
    return true;
}

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

/* A copied control callback owns native objects. Native unregister/close
 * must release it, otherwise the old QMI transports survive every reset.
 * Release outside our lock: native destructors can run immediately. */
static int kc_release_events(void *self)
{
    void *old=NULL;
    pthread_mutex_lock(&control_events_lock);
    if(control_owner==self){old=control_events;control_events=NULL;control_owner=NULL;}
    pthread_mutex_unlock(&control_events_lock);
    if(old)Block_release(old);
    os_log(OS_LOG_DEFAULT,"hakios: cc: control-lifecycle-v2 unregistered released=%d",old!=NULL);
    return IORET_OK;
}
static int
kc_events(void *self, void *queue, void *block)
{
    (void)queue;void *copied=block?Block_copy(block):NULL,*old=NULL;
    pthread_mutex_lock(&control_events_lock);
    old=control_events;control_events=copied;control_owner=self;
    pthread_mutex_unlock(&control_events_lock);
    if(old)Block_release(old);
    os_log(OS_LOG_DEFAULT,"hakios: cc: control-lifecycle-v2 registered=%d previous-released=%d",copied!=NULL,old!=NULL);
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

/* Isolated SDM845 compatibility diagnostic. The original ActivationInfo
 * update is synchronous and copies from a const borrowed input (verified at
 * 100843a98..100844390). Its complete size is 0x220, confirmed by the native
 * constructor. Borrow all real input fields for this one synchronous call;
 * change only local ActivationMode to native kDisabled=1 and mark it present.
 * No ownership is transferred; do not destroy this borrowed byte view.
 * Original status/error/identity fields and modem responses stay untouched.
 * This invokes native recalculation/notifications on the existing owner queue,
 * unlike run14's mutation from a getter. It is not an Apple activation record.
 */
/* RoamingNetworkHandler's native MCC/MNC callback consumes two consecutive
 * const std::string values. Observe size metadata only and forward both exact
 * arguments. Never replace strings, queues, ownership or native notifications.
 * Layout reads are the same size fields used by native code at 100ddffc8.
 */
static void (*native_country_update)(void *,const void *);
static size_t borrowed_native_string_size(const unsigned char *s)
{
    if((signed char)s[23]>=0)return s[23];
    size_t n=0;memcpy(&n,s+8,sizeof(n));return n;
}
static void observe_native_country_update(void *self,const void *values)
{
    if(values) {
        const unsigned char *p=values;
        os_log(OS_LOG_DEFAULT,"hakios: cc: subscriber-notify-v1 mcc-length=%zu mnc-length=%zu",
            borrowed_native_string_size(p),borrowed_native_string_size(p+24));
    }
    native_country_update(self,values);
}
/* Native CarrierBundleInterface::getCarrierBundleTechType(slot), exact
 * original integer return and arguments. Observe only; no technology override.
 */
static int (*native_bundle_technology)(void *,int);
static int observe_bundle_technology(void *self,int slot)
{
    int value=native_bundle_technology(self,slot);
    os_log(OS_LOG_DEFAULT,"hakios: cc: bundle-type-v1 slot=%d native-type=%d",slot,value);
    return value;
}
extern void probe_native_activation_dictionary(void *,uintptr_t);
static uintptr_t native_dictionary_getter;
extern void probe_native_carrier_dictionary(void *,uintptr_t);
extern void probe_borrowed_carrier_dictionary(const void *,int);
static uintptr_t native_carrier_dictionary_getter;
extern void configure_real_sim_carrier_publisher(uintptr_t,uintptr_t);
extern void remember_native_carrier_publisher(void *);
extern void publish_real_sim_carrier(void *,uintptr_t,bool,bool);
static void (*native_carrier_register)(void *);
static void register_native_carrier_publisher(void *self)
{
    native_carrier_register(self);
    remember_native_carrier_publisher(self);
}

static void (*native_carrier_publish)(void *,int,const void *);
static void observe_native_carrier_publish(void *self,int kind,const void *dictionary)
{
    probe_borrowed_carrier_dictionary(dictionary,kind);
    native_carrier_publish(self,kind,dictionary);
}
static uintptr_t native_main_slide;
/* ActivationInfo byte8 is primary BasebandServiceProvisionedState,
 * not subscriber::SimState. Verified native logging call10107dab4 resolves
 * to asString31BasebandServiceProvisionedState via Mach-O indirect symbols.
 * Map only Unknown0 to Provisioned2, with BOTH actual kernel NAS registration
 * and carrier PDP up. Preserve all explicit negative/in-progress states and
 * secondary state. This is modem service adaptation, not Apple activation.
 */
static int real_modem_integer(const char *name);
static int real_data_evidence(void);
static unsigned char primary_service_from_real_modem(unsigned char incoming,bool registered,bool pdp)
{
    return incoming==0 && registered && pdp ? 2 : incoming;
}
/* Native updater checks input+0xc before consuming ActivationStatus.
 * If absent, accept only its exact native constructor defaults: State2 and
 * ManifestResult3 at0/4, verified ActivationStatusC1 at1ef74d318 and constant
 *1ef7d9da8. They remain unchanged, as do slot2 and every negative state.
 * Marking the block present conveys real modem provisioning plus the original
 * default NotProvisioned state and Unknown manifest; no activation success or
 * ticket is fabricated. State2 name verified from native asString table.
 */
static bool supply_real_primary_service(unsigned char *info,bool registered,bool pdp)
{
    if(primary_service_from_real_modem(info[8],registered,pdp)==info[8])return false;
    int state=0,manifest=0;memcpy(&state,info,4);memcpy(&manifest,info+4,4);
    if(!info[12] && (state!=2 || manifest!=3))return false;
    info[8]=2;info[12]=1;return true;
}
static void (*native_activation_update)(void *, const void *);
static void generic_modem_activation_update(void *self, const void *info)
{
    if (!self || !info) { native_activation_update(self,info); return; }
    unsigned char adjusted[0x220] __attribute__((aligned(16)));
    memcpy(adjusted,info,sizeof(adjusted));
    int prior=0,incoming=0,mode=1;
    memcpy(&prior,(char *)self+0x288,sizeof(prior));
    memcpy(&incoming,adjusted+0x1f8,sizeof(incoming));
    if ((prior!=0 && prior!=1) || (adjusted[0x1fc] && incoming!=0 && incoming!=1)) {
        native_activation_update(self,info); return;
    }
    memcpy(adjusted+0x1f8,&mode,sizeof(mode));
    adjusted[0x1fc]=1;
    unsigned char original_primary=adjusted[8],original_secondary=adjusted[9];
    bool actual_registered=real_modem_integer("kern.hakios.cellular_registered")==1;
    bool actual_pdp=real_data_evidence()==1;
    unsigned char original_status_present=adjusted[12];
    bool service_supplied=supply_real_primary_service(adjusted,actual_registered,actual_pdp);
    unsigned char allowed_before=*((unsigned char *)self+0x338);
    bool sim_supplied=supply_real_sim_info(adjusted);
    if(sim_supplied)os_log(OS_LOG_DEFAULT,"hakios: cc: sim-info-v2 supplied missing native fields from actual SIM reads");
    native_activation_update(self,adjusted);
    int source_state=0,source_manifest=0;memcpy(&source_state,adjusted,4);memcpy(&source_manifest,adjusted+4,4);
    os_log(OS_LOG_DEFAULT,"hakios: cc: service-state-v3 supplied=%d original-present=%u native-present=%u original-state=%d original-manifest=%d",service_supplied,original_status_present,*((unsigned char *)self+0x6c),source_state,source_manifest);

    os_log(OS_LOG_DEFAULT,"hakios: cc: service-state-v2 original-primary=%u mapped-primary=%u original-secondary=%u native-primary=%u native-secondary=%u actual-registered=%d actual-pdp=%d",original_primary,adjusted[8],original_secondary,*((unsigned char *)self+0x68),*((unsigned char *)self+0x69),actual_registered,actual_pdp);

    os_log(OS_LOG_DEFAULT,"hakios: cc: sim-model-v2 card-valid=%u card-copied=%d",*((unsigned char *)self+0xeb),memcmp((unsigned char *)self+0xe1,adjusted+0x81,10)==0);

    os_log(OS_LOG_DEFAULT,"hakios: cc: subscriber-dictionary-v1 native-update-caller=%#lx",(unsigned long)((uintptr_t)__builtin_return_address(0)-native_main_slide));
    if(sim_supplied && native_dictionary_getter)probe_native_activation_dictionary(self,native_dictionary_getter);
    if(sim_supplied && native_carrier_dictionary_getter) {
        probe_native_carrier_dictionary(self,native_carrier_dictionary_getter);
        publish_real_sim_carrier(self,native_carrier_dictionary_getter,actual_registered,actual_pdp);
    }

    os_log(OS_LOG_DEFAULT,"hakios: cc: sim-model-v1 info=%u primary=%u value=%u copied=%d",
        *((unsigned char *)self+0x238),*((unsigned char *)self+0x180),*((unsigned char *)self+0xe0),
        memcmp((unsigned char *)self+0xd8,adjusted+0x78,8)==0);

    int observed=0;memcpy(&observed,(char *)self+0x288,sizeof(observed));
    static atomic_uint count=0;
    unsigned n=atomic_fetch_add(&count,1)+1;
    if(n<=8 || observed!=1)
        os_log(OS_LOG_DEFAULT,"hakios: cc: native mode update call=%u prior=%d incoming=%d mode=%d allowed=%u->%u original-status-retained=1",n,prior,incoming,observed,allowed_before,*((unsigned char *)self+0x338));
}

/* The controller also consumes the original optional fields after calling
 * the model update. Refresh its owned ActivationInfo through that same native
 * synchronous updater BEFORE returning the stable original getInfo reference.
 * Never return a stack/TLS byte view or mutate an object through a const ref.
 * Original getter always returns self+0x60 (verified8 bytes at10084447c).
 * These raw model methods are called on the controller's existing owner queue.
 * A thread-local recursion guard preserves normal reads during notifications.
 */
static const void *(*native_owned_info_getter)(void *);
static _Thread_local bool refreshing_native_info;
static const void *refresh_owned_native_info(void *self,bool registered,bool pdp)
{
    const void *original=native_owned_info_getter(self);
    if(!self || !original || refreshing_native_info || !native_activation_update)return original;
    unsigned char adjusted[0x220] __attribute__((aligned(16)));
    memcpy(adjusted,original,sizeof(adjusted));
    bool sim_changed=supply_real_sim_info(adjusted);
    bool service_changed=supply_real_primary_service(adjusted,registered,pdp);
    if(sim_changed || service_changed) {
        refreshing_native_info=true;
        native_activation_update(self,adjusted);
        if(native_carrier_dictionary_getter) {
            probe_native_carrier_dictionary(self,native_carrier_dictionary_getter);
            publish_real_sim_carrier(self,native_carrier_dictionary_getter,registered,pdp);
        }
        refreshing_native_info=false;
        const void *owned=native_owned_info_getter(self);
        os_log(OS_LOG_DEFAULT,"hakios: cc: upstream-info-v1 real-sim-updated=%d real-service-updated=%d original-owned-reference=%d",sim_changed,service_changed,owned==original);
        return owned;
    }
    return original;
}
static const void *read_owned_native_info(void *self)
{
    return refresh_owned_native_info(self,
        real_modem_integer("kern.hakios.cellular_registered")==1,
        real_data_evidence()==1);
}

/* Original driver callback1008a5ac0 retains its weak owner, calls the
 * controller synchronously with const ActivationInfo, and returns. Its two
 * native driver-delegate vtable entries are UUID/code guarded. Keep the exact
 * delegate owner and original callback, borrow all other fields only for this
 * synchronous call. No callback, object lifetime or result code is invented.
 */
static void (*native_activation_callback)(void *,const void *);
static void forward_real_activation_info(void *closure,const void *info,bool registered,bool pdp)
{
    if(!info){native_activation_callback(closure,info);return;}
    unsigned char adjusted[0x220] __attribute__((aligned(16)));
    memcpy(adjusted,info,sizeof(adjusted));
    bool sim_changed=supply_real_sim_info(adjusted);
    bool service_changed=supply_real_primary_service(adjusted,registered,pdp);
    os_log(OS_LOG_DEFAULT,"hakios: cc: upstream-callback-v1 actual-callback=1 real-sim-updated=%d real-service-updated=%d original-input-preserved=1",sim_changed,service_changed);
    native_activation_callback(closure,adjusted);
}
static void receive_real_activation_info(void *closure,const void *info)
{
    forward_real_activation_info(closure,info,
        real_modem_integer("kern.hakios.cellular_registered")==1,
        real_data_evidence()==1);
}

/* Generic modem service evidence, separate from Apple device activation.
 * Unknown vendor provisioning cannot describe this modem's service. Only map
 * kUnknown=0 to kProvisioned=2 when the real SDM845 NAS registration AND its
 * real carrier PDP session are up. Preserve explicit negative/in-progress
 * states and every original modem reply. This is local cellular service
 * adaptation; it creates no Apple tickets, account tokens or device identity.
 */
static void (*native_service_provisioned)(void *,int);
static int real_modem_integer(const char *name)
{
    int value=0;size_t size=sizeof(value);
    return sysctlbyname(name,&value,&size,NULL,0)==0 && size==sizeof(value) ? value:0;
}
/* modem_native_pdp=1: the kernel opens no data call of its own (CommCenter's are the only ones), so its
 * cellular_data stays 0; there the evidence is the registration alone, or CommCenter would wait for a call
 * it has to make itself first (cc-native-47/48: EmergencyOnly). */
static int native_pdp_mode(void)
{
    static int native=-1;
    if(native<0){int value=0;size_t value_size=sizeof(value);
        if(sysctlbyname("kern.hakios.native_pdp",&value,&value_size,NULL,0)==0)native=value!=0;
        else{char args[1024]={0};size_t size=sizeof(args)-1;
            native=sysctlbyname("kern.bootargs",args,&size,NULL,0)==0 && strstr(args," modem_native_pdp=1")!=NULL;}}
    return native;
}
static int real_data_evidence(void)
{
    if(native_pdp_mode())return real_modem_integer("kern.hakios.cellular_registered");
    return real_modem_integer("kern.hakios.cellular_data");
}
static void generic_service_provisioned(void *self,int incoming)
{
    int registered=real_modem_integer("kern.hakios.cellular_registered");
    int data=real_data_evidence();
    int mapped=incoming==0 && registered==1 && data==1 ? 2:incoming;
    /* modem_native_pdp=1: this modem has no Apple baseband provisioning; while it is registered, the state is
     * provisioned. A carrier bundle change sets it to 4 for its file push (which this modem refuses), and CommCenter
     * answered that by reporting EmergencyOnly and ending the data calls for good (cc-native-53). An unregistered
     * modem (no SIM, no network) still passes the vendor's state through. */
    if(native_pdp_mode() && registered==1)mapped=2;
    os_log(OS_LOG_DEFAULT,"hakios: cc: generic-service-v1 vendor-state=%d real-registered=%d real-pdp=%d mapped=%d",incoming,registered,data,mapped);
    native_service_provisioned(self,mapped);
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
    /* OSLog selects its sink at process start. In a RAM boot the resident
     * system's launchd plist is used, not this small image's plist copy.
     * Re-exec once before changing any bridge state; the marker prevents loops.
     * The original CommCenter argument list has no extra options. */
    /* (only with cc_diag=1: CommCenter's log to /private/var/tmp/cc-stderr.txt, for share/ccprobe; some MB a run) */
    char diag_args[1024]={0};size_t diag_size=sizeof(diag_args)-1;
    bool diag=sysctlbyname("kern.bootargs",diag_args,&diag_size,NULL,0)==0 && strstr(diag_args," cc_diag=1")!=NULL;
    if(diag && getenv("HAKIOS_CC_DIAG_REEXEC")==NULL) {
        if(setenv("OS_ACTIVITY_DT_MODE","YES",1)==0 && setenv("HAKIOS_CC_DIAG_REEXEC","1",1)==0) {
            char *arguments[]={"/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter",NULL};
            execv(arguments[0],arguments);
        }
    }
    int output=diag ? open("/private/var/tmp/cc-stderr.txt",O_WRONLY|O_CREAT|O_TRUNC,0644) : -1;
    if(output>=0){dup2(output,2);dup2(output,1);close(output);}
    setvbuf(stderr,NULL,_IONBF,0);setvbuf(stdout,NULL,_IOLBF,0);
    fprintf(stderr,"hakios: cc: diag-v45 current-process=%d reexec-marker=%d\n",getpid(),getenv("HAKIOS_CC_DIAG_REEXEC")!=NULL);
    struct stat carrier_stat;
    bool carrier_default=stat("/System/Library/Carrier Bundles/iPhone/Default.bundle/carrier.plist",&carrier_stat)==0 && carrier_stat.st_size>0;
    bool carrier_fet=stat("/System/Library/Carrier Bundles/iPhone/46601/carrier.plist",&carrier_stat)==0 && carrier_stat.st_size>0;
    fprintf(stderr,"hakios: cc: carrier-resources-v2 default-readable=%d fet-readable=%d\n",carrier_default,carrier_fet);
    bool default_link=stat("/var/mobile/Library/Carrier Bundles/BundleLinks/CarrierDefault.bundle/carrier.plist",&carrier_stat)==0 && carrier_stat.st_size>0;
    fprintf(stderr,"hakios: cc: carrier-bootstrap-v1 default-link-readable=%d\n",default_link);


	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	/* (the vtable is in the cache, which is mapped whole whether libBasebandManager has been loaded yet or not) */
	if (length == 0) {
		os_log(OS_LOG_DEFAULT, "hakios: cc: no shared cache: nothing changed");
		return;
	}
    char generic_args[2048]={0};size_t generic_size=sizeof(generic_args)-1;
    if(sysctlbyname("kern.bootargs",generic_args,&generic_size,NULL,0)==0 &&
       (strstr(generic_args," cc_generic_modem_mode=1")!=NULL || native_pdp_mode())) {
        const struct mach_header_64 *executable=NULL;
        uintptr_t executable_slide=0;unsigned candidates=0;bool uuid_match=false;
        for(uint32_t j=0;j<_dyld_image_count();j++) {
            const struct mach_header_64 *h=(const struct mach_header_64 *)_dyld_get_image_header(j);
            const char *name=_dyld_get_image_name(j);
            if(h && h->magic==MH_MAGIC_64 && h->filetype==MH_EXECUTE && name && strstr(name,"/CommCenter")) {
                executable=h;executable_slide=(uintptr_t)_dyld_get_image_vmaddr_slide(j);candidates++;
            }
        }
        static const unsigned char expected_uuid[16]={0x5a,0x90,0x7e,0x0e,0xbf,0x03,0x37,0x6b,0x94,0xe6,0xe7,0x37,0xbd,0xd4,0x03,0x00};
        if(candidates==1 && executable->sizeofcmds<65536 && executable->ncmds<512 &&
           (uintptr_t)executable==0x100000000UL+executable_slide) {
            const unsigned char *command=(const unsigned char *)(executable+1),*end=command+executable->sizeofcmds;
            for(unsigned j=0;j<executable->ncmds && command+sizeof(struct load_command)<=end;j++) {
                const struct load_command *lc=(const struct load_command *)command;
                if(lc->cmdsize<sizeof(*lc) || lc->cmdsize>(size_t)(end-command))break;
                if(lc->cmd==LC_UUID && lc->cmdsize>=sizeof(struct uuid_command)) {
                    const unsigned char *u=((const struct uuid_command *)lc)->uuid;
                    uuid_match=memcmp(u,expected_uuid,16)==0;
                    char id[33];for(unsigned k=0;k<16;k++)snprintf(id+k*2,3,"%02x",u[k]);
                    os_log(OS_LOG_DEFAULT,"hakios: cc: actual executable uuid=%{public}s expected-match=%d",id,uuid_match);
                }
                command+=lc->cmdsize;
            }
        }
        int bundle_observer_installed=0;
        if(uuid_match) {
            uintptr_t *entry=(uintptr_t *)(0x1015713c8UL+executable_slide);
            uintptr_t expected=0x1004f8214UL+executable_slide;
            static const unsigned char code[16]={0xff,0x83,0x02,0xd1,0xfd,0x7b,0x09,0xa9,0xfd,0x43,0x02,0x91,0xe1,0x1f,0x00,0xb9};
            if(*entry==expected && memcmp((void *)expected,code,16)==0) {
                native_bundle_technology=(int (*)(void *,int))expected;
                bundle_observer_installed=write_slot(entry,(uintptr_t)observe_bundle_technology);
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: bundle-type-v1 observer-installed=%d",bundle_observer_installed);
        int subscriber_observer_installed=0;
        if(uuid_match) {
            uintptr_t *entry=(uintptr_t *)(0x1015fb980UL+executable_slide);
            uintptr_t expected=0x100ddff88UL+executable_slide;
            static const unsigned char code[16]={0xff,0x43,0x01,0xd1,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9,0xfd,0x7b,0x04,0xa9};
            if(*entry==expected && memcmp((void *)expected,code,16)==0) {
                native_country_update=(void (*)(void *,const void *))expected;
                subscriber_observer_installed=write_slot(entry,(uintptr_t)observe_native_country_update);
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: subscriber-notify-v1 observer-installed=%d",subscriber_observer_installed);
        int changed=0;
        if(uuid_match) {
            uintptr_t *entry=(uintptr_t *)(0x1015a5ad0UL+executable_slide);
            uintptr_t expected=0x100843a98UL+executable_slide;
            static const unsigned char expected_code[16]={0xff,0xc3,0x02,0xd1,0xf8,0x5f,0x07,0xa9,0xf6,0x57,0x08,0xa9,0xf4,0x4f,0x09,0xa9};
            bool code_matches=memcmp((const void *)expected,expected_code,sizeof(expected_code))==0;
            os_log(OS_LOG_DEFAULT,"hakios: cc: actual executable main-slide=%#lx function-match=%d vtable-match=%d",(unsigned long)executable_slide,code_matches,*entry==expected);
            if(*entry==expected && code_matches) {
                uintptr_t getter=0x100841bf8UL+executable_slide;
                static const unsigned char getter_code[16]={0xff,0x43,0x02,0xd1,0xf8,0x5f,0x05,0xa9,0xf6,0x57,0x06,0xa9,0xf4,0x4f,0x07,0xa9};
                if(*(uintptr_t *)(0x1015a5a80UL+executable_slide)==getter && memcmp((void *)getter,getter_code,16)==0)
                    native_dictionary_getter=getter;
                native_main_slide=executable_slide;
                native_activation_update=(void (*)(void *,const void *))expected;
                changed=write_slot(entry,(uintptr_t)generic_modem_activation_update);
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: generic modem native update adapter installed=%d main-candidates=%u",changed,candidates);
        int carrier_lifetime_hook=0;
        if(uuid_match) {
            uintptr_t expected=0x100579490UL+executable_slide;
            static const unsigned char code[16]={0xff,0xc3,0x00,0xd1,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9,0xfd,0x83,0x00,0x91};
            uintptr_t *entry=(uintptr_t *)(0x1015782d8UL+executable_slide);
            uintptr_t publisher=0x10057a554UL+executable_slide;
            static const unsigned char publisher_code[16]={0xff,0x43,0x06,0xd1,0xe9,0x23,0x12,0x6d,0xfc,0x6f,0x13,0xa9,0xfa,0x67,0x14,0xa9};
            uintptr_t *publisher_entry=(uintptr_t *)(0x1015782f0UL+executable_slide);
            if(*entry==expected && memcmp((void *)expected,code,16)==0 && *publisher_entry==publisher && memcmp((void *)publisher,publisher_code,16)==0) {
                native_carrier_register=(void (*)(void *))expected;
                configure_real_sim_carrier_publisher(0x1015782c0UL+executable_slide,0x10057a554UL+executable_slide);
                carrier_lifetime_hook=write_slot(entry,(uintptr_t)register_native_carrier_publisher);
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: real-sim-carrier-v1 lifetime-hook=%d",carrier_lifetime_hook);
        /*
         * BasebandSettings::resetBBAfterFileTransferComplete_sync(bool) (0x100671ea4, called directly from two places):
         * after a carrier bundle change it pushes the carrier's PRI/GRI files to the baseband -- which this modem
         * refuses ("Pushing FileType::FILETYPE_PRI failed, soldiering on") -- and then resets the baseband. A reset of
         * the stand-ins leaves the transport unable to engage again ("Engage failure" every 10 s), and the data calls
         * are gone for good (cc-native-52: "Reset requested ... Resetting baseband after pushing files (due to
         * kCarrierBundleChange)" 3 minutes after the start). Nothing was pushed, so nothing is to be applied: the
         * function returns at once. UUID and prologue checked; the page copied (VM_PROT_COPY), not the file.
         */
        int file_reset_skipped=0;
        if(uuid_match) {
            uint32_t *entry=(uint32_t *)(0x100671ea4UL+executable_slide);
            static const unsigned char prologue[16]={0xff,0xc3,0x06,0xd1,0xf8,0x5f,0x17,0xa9,0xf6,0x57,0x18,0xa9,0xf4,0x4f,0x19,0xa9};
            if(memcmp(entry,prologue,16)==0) {
                vm_address_t page=(vm_address_t)entry & ~(vm_address_t)(vm_page_size-1);
                kern_return_t kr=vm_protect(mach_task_self(),page,vm_page_size,false,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
                if(kr==KERN_SUCCESS) {
                    *entry=0xd65f03c0u;   /* ret */
                    kr=vm_protect(mach_task_self(),page,vm_page_size,false,VM_PROT_READ|VM_PROT_EXECUTE);
                    sys_icache_invalidate(entry,4);
                    file_reset_skipped=kr==KERN_SUCCESS ? 1:-2;
                } else file_reset_skipped=-1;
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: carrier-file-reset-v1 skipped=%d",file_reset_skipped);
        fprintf(stderr,"hakios: cc: carrier-file-reset-v1 skipped=%d\n",file_reset_skipped);
        int carrier_observer_installed=0;
        if(uuid_match) {
            uintptr_t getter=0x100843160UL+executable_slide;
            static const unsigned char getter_code[16]={0xff,0x83,0x02,0xd1,0xfa,0x67,0x05,0xa9,0xf8,0x5f,0x06,0xa9,0xf6,0x57,0x07,0xa9};
            if(memcmp((void *)getter,getter_code,16)==0)native_carrier_dictionary_getter=getter;
            uintptr_t expected=0x10057a554UL+executable_slide;
            static const unsigned char publish_code[16]={0xff,0x43,0x06,0xd1,0xe9,0x23,0x12,0x6d,0xfc,0x6f,0x13,0xa9,0xfa,0x67,0x14,0xa9};
            uintptr_t *entry=(uintptr_t *)(0x1015782f0UL+executable_slide);
            if(*entry==expected && memcmp((void *)expected,publish_code,16)==0) {
                native_carrier_publish=(void (*)(void *,int,const void *))expected;
                carrier_observer_installed=write_slot(entry,(uintptr_t)observe_native_carrier_publish);
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: carrier-publish-v1 observer-installed=%d aggregate-getter=%d original-values-preserved=1",carrier_observer_installed,native_carrier_dictionary_getter!=0);
        int input_callback_hooks=0;
        if(uuid_match) {
            uintptr_t expected=0x1008a5ac0UL+executable_slide;
            static const unsigned char code[16]={0xff,0x03,0x01,0xd1,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,0xfd,0x7b,0x03,0xa9};
            if(memcmp((void *)expected,code,16)==0) {
                native_activation_callback=(void (*)(void *,const void *))expected;
                const uintptr_t locations[]={0x1015ae2a8UL,0x1015ae318UL};
                for(unsigned i=0;i<2;i++) {
                    uintptr_t *entry=(uintptr_t *)(locations[i]+executable_slide);
                    if(*entry==expected)input_callback_hooks+=write_slot(entry,(uintptr_t)receive_real_activation_info);
                }
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: upstream-callback-v1 hooks=%d expected=2",input_callback_hooks);
        int owned_info_adapter_installed=0;
        if(uuid_match && native_activation_update) {
            uintptr_t *entry=(uintptr_t *)(0x1015a5ad8UL+executable_slide);
            uintptr_t expected=0x10084447cUL+executable_slide;
            static const unsigned char code[8]={0x00,0x80,0x01,0x91,0xc0,0x03,0x5f,0xd6};
            if(*entry==expected && memcmp((void *)expected,code,8)==0) {
                native_owned_info_getter=(const void *(*)(void *))expected;
                owned_info_adapter_installed=write_slot(entry,(uintptr_t)read_owned_native_info);
            }
        }
        os_log(OS_LOG_DEFAULT,"hakios: cc: upstream-info-v1 adapter-installed=%d",owned_info_adapter_installed);

        if(uuid_match && (strstr(generic_args," cc_generic_modem_provision=1") || native_pdp_mode())) {
            uintptr_t *entry=(uintptr_t *)(0x10158ea30UL+executable_slide);
            uintptr_t expected=0x1006eb398UL+executable_slide;
            static const unsigned char code[16]={0xff,0x43,0x01,0xd1,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9,0xfd,0x7b,0x04,0xa9};
            bool matches=*entry==expected && memcmp((void *)expected,code,16)==0;
            int installed=0;
            if(matches) {
                native_service_provisioned=(void (*)(void *,int))expected;
                installed=write_slot(entry,(uintptr_t)generic_service_provisioned);
            }
            os_log(OS_LOG_DEFAULT,"hakios: cc: generic-service-v1 installed=%d guarded-code=%d",installed,matches);
        }

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
        /* Keep native BasebandTransport setup: FSModule consumes the callbacks
         * that open() builds in that object. Only the kernel-facing acipc layer
         * is replaced. The old broad hook returned true without those callbacks. */
        (void)transport_open; (void)transport_write; (void)transport_read; (void)transport_close;
        os_log(OS_LOG_DEFAULT, "hakios: cc: native BasebandTransport initialization retained");
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
        int lifetime_hooks=0;
        lifetime_hooks+=take_over(slide,VT_ACIPC,AK_DTOR,(void *)ak_destructor);
        lifetime_hooks+=take_over(slide,VT_ACIPC,AK_DELETE,(void *)ak_deleting_destructor);
        os_log(OS_LOG_DEFAULT,"hakios: cc: lifecycle-v1 destructor hooks=%d expected=2",lifetime_hooks);
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

		taken += take_over(slide, VT_ACIPC_CTL, KC_START, (void *)kc_start);
		taken += take_over(slide, VT_ACIPC_CTL, KC_STOP, (void *)kc_release_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_OPEN_RL, (void *)kc_open);
		taken += take_over(slide, VT_ACIPC_CTL, KC_OPEN_Q, (void *)kc_open);
		taken += take_over(slide, VT_ACIPC_CTL, KC_CLOSE, (void *)kc_release_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_READ_LOGS, (void *)kc_unsupported);
		taken += take_over(slide, VT_ACIPC_CTL, KC_EVENTS_RL, (void *)kc_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_EVENTS_Q, (void *)kc_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_NO_EVENTS, (void *)kc_release_events);
		taken += take_over(slide, VT_ACIPC_CTL, KC_LOG_SIZE, (void *)kc_unsupported);
		taken += take_over(slide, VT_ACIPC_CTL, KC_LOG_TUNE, (void *)kc_ok);
		taken += take_over(slide, VT_ACIPC_CTL, KC_BOOT_STAGE, (void *)kc_boot_stage);
		os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 12 of acipcKernelControl's slots answered here", taken);
	}
	os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 2 of BootControllerLegacy's slots answer \"booted\" (slide %#lx; slots 2-5 %#lx %#lx %#lx %#lx)",
	    done, (unsigned long)slide, (unsigned long)(vtable[2] - slide), (unsigned long)(vtable[3] - slide),
	    (unsigned long)(vtable[4] - slide), (unsigned long)(vtable[5] - slide));
}
