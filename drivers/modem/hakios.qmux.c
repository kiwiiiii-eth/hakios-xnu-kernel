/*
 * /dev/hakios-qmux0..3: QMUX, as an iPhone's Apple Baseband Manager speaks it to its modem, over this phone's QRTR (driver
 * BASEBAND, abb=1).
 *
 * ABM (libBasebandManager, inside CommCenter) talks QMI to a Qualcomm modem through QMux channels (MAV_ROUTER_QMI_0, _1),
 * each a byte pipe over the PCIe transport of an iPhone's modem. This phone's modem speaks the same QMI, over QRTR on the
 * SoC (hakios.mpss845.c). Each of these devices is one such pipe (userland/src/inject_cc.c puts it where ABM's transport
 * was): a write is one QMUX message, a read gives one.
 *
 *   QMUX message:  0x01 (interface type), its length (16 bits, all that follows the 0x01), control flags (0x00 from
 *                  the host, 0x80 from the modem), the service type, the client's number, then the service's SDU
 *   CTL's SDU:     flags (0 request, 1 response, 2 indication), transaction (8 bits), message (16), length (16), TLVs
 *   a service's:   flags (0 request, 2 response, 4 indication), transaction (16 bits), message, length, TLVs -- byte
 *                  for byte a QMI message as QRTR carries it
 *
 * QRTR has no CTL (service 0): it is answered here. A client asked for (GET_CLIENT_ID) is a QRTR port of our own looking
 * the service up; its messages go to the service's server as they are, and what comes back (responses, indications)
 * comes back with the client's QMUX header. A service the modem has not got (Apple's own: 0xe1 audio, 0xe2, 0xe7, ...)
 * is refused its client.
 */
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/hakios.qrtr.h>
#include <pexpert/pexpert.h>

extern void loader_log_putc(char c);

#define QMUX_DEVICES            4
#define QMUX_CLIENTS            128
#define QMUX_LARGEST            8192
#define QMUX_RING               65536u
#define QMUX_FIND_MS            1500            /* how long a service's server is looked for */

struct qmux_device;

struct qmux_client {
	struct qmux_device      *device;
	struct hakios_qrtr_port *port;
	uint32_t                node, server_port;
	uint8_t                 service, id;
	bool                    used, found;
};

struct qmux_device {
	bool            open;
	uint8_t         ring[QMUX_RING];
	uint32_t        head, tail;             /* bytes written, read (free-running); each message is its length (2) and itself */
	uint32_t        dropped, written, read_count;
	uint8_t         next_id[256];
	unsigned        said;
};

static struct qmux_device devices[QMUX_DEVICES];
static struct qmux_client clients[QMUX_CLIENTS];
static LCK_GRP_DECLARE(qmux_group, "hakios-qmux");
static LCK_MTX_DECLARE(qmux_lock, &qmux_group);
static int qmux_major = -1;

static void qmux_say(const char *format, ...) __printflike(1, 2);

static void
qmux_say(const char *format, ...)
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

/*
 * boot-arg qmux_wds=1: each message of the data services (WDS 0x01, WDS admin 0x1a), both ways, with its TLVs -- what a
 * data call asked for (its family, its APN, the mux it was bound to), what the modem answered, and why a call ended. The
 * packet statistics CommCenter polls for are left out; at most QMUX_TRACE_LINES lines a boot.
 */
#define QMUX_TRACE_LINES        900
static uint32_t qmux_trace_on, qmux_trace_said;

static void
qmux_trace(bool to_modem, uint8_t service, uint8_t client, const uint8_t *sdu, uint32_t length)
{
	char line[300];
	uint32_t at, off, size, tl;
	uint16_t message;

	if (!qmux_trace_on || (service != 0x01 && service != 0x1a) || length < 7) {
		return;
	}
	message = (uint16_t)(sdu[3] | sdu[4] << 8);
	if (message == 0x0024 || qmux_trace_said >= QMUX_TRACE_LINES) {
		return;
	}
	qmux_trace_said++;
	tl = (uint32_t)(sdu[5] | sdu[6] << 8);
	at = (uint32_t)snprintf(line, sizeof(line), "qmux-wds: %s %#x/%u %s %u msg %#06x:", to_modem ? ">" : "<", service, client,
	    sdu[0] == 0 ? "req" : sdu[0] == 2 ? "resp" : sdu[0] == 4 ? "ind" : "?", (unsigned)(sdu[1] | sdu[2] << 8), message);
	for (off = 7; off + 3 <= length && off < 7 + tl && at < sizeof(line) - 12; off += 3 + size) {
		size = (uint32_t)(sdu[off + 1] | sdu[off + 2] << 8);
		at += (uint32_t)snprintf(line + at, sizeof(line) - at, " %02x/%u=", sdu[off], size);
		for (uint32_t i = 0; i < size && i < 24 && off + 3 + i < length && at < sizeof(line) - 4; i++) {
			at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x", sdu[off + 3 + i]);
		}
	}
	qmux_say("%s\n", line);
}

/* With the lock held: one message for the device's reader, or counted as dropped. */
static void
qmux_put(struct qmux_device *d, const uint8_t *message, uint32_t length)
{
	uint8_t size[2] = { (uint8_t)length, (uint8_t)(length >> 8) };

	if (QMUX_RING - (d->head - d->tail) < length + 2) {
		d->dropped++;
		return;
	}
	for (uint32_t i = 0; i < 2; i++) {
		d->ring[(d->head + i) % QMUX_RING] = size[i];
	}
	for (uint32_t i = 0; i < length; i++) {
		d->ring[(d->head + 2 + i) % QMUX_RING] = message[i];
	}
	d->head += length + 2;
	wakeup(d);
}

/* a QMUX header (6 bytes) before an SDU of LENGTH bytes */
static void
qmux_header(uint8_t *m, uint32_t length, uint8_t service, uint8_t client)
{
	uint32_t whole = 5 + length;

	m[0] = 0x01;
	m[1] = (uint8_t)whole;
	m[2] = (uint8_t)(whole >> 8);
	m[3] = 0x80;
	m[4] = service;
	m[5] = client;
}


/* ---- what the modem says to a client: back to the device, with the client's QMUX header ---- */

static void
qmux_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct qmux_client *c = context;
	uint8_t *message;

	(void)node;
	(void)port;
	if (length > QMUX_LARGEST || length < 7 || (message = kalloc_data(6 + length, Z_WAITOK)) == NULL) {
		return;
	}
	lck_mtx_lock(&qmux_lock);
	if (c->used && c->device != NULL && c->device->open) {
		qmux_trace(false, c->service, c->id, data, (uint32_t)length);
		qmux_header(message, (uint32_t)length, c->service, c->id);
		memcpy(message + 6, data, length);
		qmux_put(c->device, message, (uint32_t)length + 6);
	}
	lck_mtx_unlock(&qmux_lock);
	kfree_data(message, 6 + length);
}

static void
qmux_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port)
{
	struct qmux_client *c = context;

	(void)service;
	(void)version;
	(void)instance;
	lck_mtx_lock(&qmux_lock);
	if (arrived && !c->found) {
		c->node = node;
		c->server_port = port;
		c->found = true;
		wakeup(c);
	} else if (!arrived && c->found && c->node == node && c->server_port == port) {
		c->found = false;
	}
	lck_mtx_unlock(&qmux_lock);
}


/* ---- CTL, answered here ---- */

/* a CTL response: result, error, and TLV 0x01 of the bytes given, if any */
static void
ctl_answer(struct qmux_device *d, uint8_t transaction, uint16_t message, uint16_t result, uint16_t error,
    const uint8_t *tlv1, uint16_t tlv1_length)
{
	uint8_t m[6 + 6 + 7 + 3 + 256];
	uint8_t *s = m + 6;
	uint16_t tlvs = (uint16_t)(7 + (tlv1 != NULL ? 3 + tlv1_length : 0));

	s[0] = 0x01;                            /* response */
	s[1] = transaction;
	s[2] = (uint8_t)message;
	s[3] = (uint8_t)(message >> 8);
	s[4] = (uint8_t)tlvs;
	s[5] = (uint8_t)(tlvs >> 8);
	s[6] = 0x02;                            /* result */
	s[7] = 4;
	s[8] = 0;
	s[9] = (uint8_t)result;
	s[10] = (uint8_t)(result >> 8);
	s[11] = (uint8_t)error;
	s[12] = (uint8_t)(error >> 8);
	if (tlv1 != NULL) {
		s[13] = 0x01;
		s[14] = (uint8_t)tlv1_length;
		s[15] = (uint8_t)(tlv1_length >> 8);
		memcpy(s + 16, tlv1, tlv1_length);
	}
	qmux_header(m, 6u + tlvs, 0, 0);
	lck_mtx_lock(&qmux_lock);
	qmux_put(d, m, 6u + 6u + tlvs);
	lck_mtx_unlock(&qmux_lock);
}

/* TLV TYPE of the TLVs at T (LENGTH bytes): its value and length, or NULL */
static const uint8_t *
tlv(const uint8_t *t, uint32_t length, uint8_t type, uint16_t *size)
{
	for (uint32_t at = 0; at + 3 <= length;) {
		uint16_t n = (uint16_t)(t[at + 1] | t[at + 2] << 8);

		if (at + 3 + n > length) {
			break;
		}
		if (t[at] == type) {
			*size = n;
			return t + at + 3;
		}
		at += 3 + n;
	}
	return NULL;
}

static void
ctl_get_client(struct qmux_device *d, uint8_t transaction, const uint8_t *tlvs, uint32_t length)
{
	const struct hakios_qrtr_ops *ops = hakios_mpss_qrtr();
	struct qmux_client *c = NULL;
	struct timespec wait = { QMUX_FIND_MS / 1000, (QMUX_FIND_MS % 1000) * 1000000 };
	uint16_t size = 0;
	const uint8_t *service = tlv(tlvs, length, 0x01, &size);
	uint8_t answer[2];

	if (service == NULL || size < 1) {
		ctl_answer(d, transaction, 0x0022, 1, 0x0030, NULL, 0);        /* (missing argument) */
		return;
	}
	if (ops == NULL) {
		qmux_say("qmux: a client of service %#x asked for, and the modem's QRTR is not up yet\n", service[0]);
		ctl_answer(d, transaction, 0x0022, 1, 0x0003, NULL, 0);        /* (internal) */
		return;
	}
	lck_mtx_lock(&qmux_lock);
	for (unsigned i = 0; i < QMUX_CLIENTS && c == NULL; i++) {
		if (!clients[i].used) {
			c = &clients[i];
			bzero(c, sizeof(*c));
			c->used = true;
			c->device = d;
			c->service = service[0];
			c->id = ++d->next_id[service[0]];
			if (c->id == 0) {
				c->id = ++d->next_id[service[0]];
			}
		}
	}
	lck_mtx_unlock(&qmux_lock);
	if (c == NULL) {
		ctl_answer(d, transaction, 0x0022, 1, 0x0005, NULL, 0);        /* (no more clients) */
		return;
	}
	c->port = ops->open(qmux_receive, qmux_server, c);
	int lookup_error = c->port == NULL ? ENOSPC : ops->lookup(c->port, c->service, 0, 0);
	if (lookup_error != 0) {
		qmux_say("qmux: service %#x: port allocated=%u lookup-error=%d clients-limit=%u\n", c->service, c->port != NULL, lookup_error, QMUX_CLIENTS);
		if (c->port != NULL) {
			ops->close(c->port);
		}
		lck_mtx_lock(&qmux_lock);
		c->used = false;
		lck_mtx_unlock(&qmux_lock);
		ctl_answer(d, transaction, 0x0022, 1, 0x0005, NULL, 0);
		return;
	}
	lck_mtx_lock(&qmux_lock);
	if (!c->found) {
		(void)msleep(c, &qmux_lock, PZERO, "hakios-qmux", &wait);
	}
	if (!c->found) {
		lck_mtx_unlock(&qmux_lock);
		qmux_say("qmux: service %#x: the modem has no server of it (client refused)\n", c->service);
		ops->close(c->port);
		lck_mtx_lock(&qmux_lock);
		c->used = false;
		lck_mtx_unlock(&qmux_lock);
		ctl_answer(d, transaction, 0x0022, 1, 0x001f, NULL, 0);        /* (invalid service type) */
		return;
	}
	answer[0] = c->service;
	answer[1] = c->id;
	lck_mtx_unlock(&qmux_lock);
	qmux_say("qmux: service %#x: client %u, the modem's server %u:%u\n", c->service, c->id, c->node, c->server_port);
	ctl_answer(d, transaction, 0x0022, 0, 0, answer, 2);
}

static void
ctl_release_client(struct qmux_device *d, uint8_t transaction, const uint8_t *tlvs, uint32_t length)
{
	const struct hakios_qrtr_ops *ops = hakios_mpss_qrtr();
	uint16_t size = 0;
	const uint8_t *which = tlv(tlvs, length, 0x01, &size);
	struct hakios_qrtr_port *port = NULL;

	if (which == NULL || size < 2) {
		ctl_answer(d, transaction, 0x0023, 1, 0x0030, NULL, 0);
		return;
	}
	lck_mtx_lock(&qmux_lock);
	for (unsigned i = 0; i < QMUX_CLIENTS; i++) {
		if (clients[i].used && clients[i].device == d && clients[i].service == which[0] && clients[i].id == which[1]) {
			port = clients[i].port;
			clients[i].used = false;
		}
	}
	lck_mtx_unlock(&qmux_lock);
	if (port != NULL && ops != NULL) {
		ops->close(port);
	}
	ctl_answer(d, transaction, 0x0023, 0, 0, which, 2);
}

/* CTL's GET_VERSION_INFO: the services a modem of this kind has, each said to be version 1.0 */
static void
ctl_versions(struct qmux_device *d, uint8_t transaction)
{
	static const uint8_t services[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0a, 0x0b, 0x0c, 0x10, 0x1a, 0x22,
		                            0x24, 0x29, 0x2a };
	uint8_t list[1 + sizeof(services) * 5];

	list[0] = sizeof(services);
	for (unsigned i = 0; i < sizeof(services); i++) {
		uint8_t *e = list + 1 + i * 5;

		e[0] = services[i];
		e[1] = 1;
		e[2] = 0;
		e[3] = 0;
		e[4] = 0;
	}
	ctl_answer(d, transaction, 0x0021, 0, 0, list, sizeof(list));
}

static void
qmux_ctl(struct qmux_device *d, const uint8_t *sdu, uint32_t length)
{
	uint8_t transaction;
	uint16_t message, size;

	if (length < 6 || sdu[0] != 0x00) {
		return;                                 /* (requests only) */
	}
	transaction = sdu[1];
	message = (uint16_t)(sdu[2] | sdu[3] << 8);
	size = (uint16_t)(sdu[4] | sdu[5] << 8);
	if ((uint32_t)size + 6 > length) {
		size = (uint16_t)(length - 6);
	}
	qmux_say("qmux: CTL message %#x (transaction %u, %u bytes of TLVs)\n", message, transaction, size);
	switch (message) {
	case 0x0022:
		ctl_get_client(d, transaction, sdu + 6, size);
		break;
	case 0x0023:
		ctl_release_client(d, transaction, sdu + 6, size);
		break;
	case 0x0021:
		ctl_versions(d, transaction);
		break;
	default:
		ctl_answer(d, transaction, message, 0, 0, NULL, 0);           /* (SET_INSTANCE_ID, SYNC, ...: done) */
		break;
	}
}


/* ---- the device ---- */

static struct qmux_device *
qmux_device(dev_t dev)
{
	int unit = minor(dev);

	return unit >= 0 && unit < QMUX_DEVICES ? &devices[unit] : NULL;
}

static int
qmux_open(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	struct qmux_device *d = qmux_device(dev);
	char name[32] = "";

	if (d == NULL) {
		return ENXIO;
	}
	lck_mtx_lock(&qmux_lock);
	if (d->open) {
		lck_mtx_unlock(&qmux_lock);
		return EBUSY;
	}
	bzero(d, sizeof(*d));
	d->open = true;
	lck_mtx_unlock(&qmux_lock);
	proc_selfname(name, sizeof(name));
	qmux_say("qmux: /dev/hakios-qmux%d opened by %s[%d]\n", minor(dev), name, proc_selfpid());
	return 0;
}

static int
qmux_close(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	struct qmux_device *d = qmux_device(dev);
	const struct hakios_qrtr_ops *ops = hakios_mpss_qrtr();
	struct hakios_qrtr_port *ports[QMUX_CLIENTS];
	unsigned n = 0;

	if (d == NULL) {
		return 0;
	}
	lck_mtx_lock(&qmux_lock);
	for (unsigned i = 0; i < QMUX_CLIENTS; i++) {
		if (clients[i].used && clients[i].device == d) {
			ports[n++] = clients[i].port;
			clients[i].used = false;
		}
	}
	d->open = false;
	wakeup(d);
	lck_mtx_unlock(&qmux_lock);
	for (unsigned i = 0; i < n && ops != NULL; i++) {
		ops->close(ports[i]);
	}
	qmux_say("qmux: /dev/hakios-qmux%d closed: %u messages written, %u read, %u dropped; %u clients let go\n", minor(dev),
	    d->written, d->read_count, d->dropped, n);
	return 0;
}

/* one message (whole, or EMSGSIZE); waits a second for one at most (EWOULDBLOCK) */
static int
qmux_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct qmux_device *d = qmux_device(dev);
	struct timespec wait = { 1, 0 };
	uint8_t *message;
	uint32_t length;
	int error;

	if (d == NULL) {
		return ENXIO;
	}
	lck_mtx_lock(&qmux_lock);
	if (d->head == d->tail) {
		error = msleep(d, &qmux_lock, PCATCH | PZERO, "hakios-qmux", &wait);
		if (d->head == d->tail) {
			lck_mtx_unlock(&qmux_lock);
			return error != 0 && error != EWOULDBLOCK ? error : EWOULDBLOCK;
		}
	}
	length = (uint32_t)(d->ring[d->tail % QMUX_RING] | d->ring[(d->tail + 1) % QMUX_RING] << 8);
	if ((user_ssize_t)length > uio_resid(uio) || length > 6 + QMUX_LARGEST) {
		lck_mtx_unlock(&qmux_lock);
		return EMSGSIZE;
	}
	lck_mtx_unlock(&qmux_lock);
	if ((message = kalloc_data(length, Z_WAITOK)) == NULL) {
		return ENOMEM;
	}
	lck_mtx_lock(&qmux_lock);
	for (uint32_t i = 0; i < length; i++) {
		message[i] = d->ring[(d->tail + 2 + i) % QMUX_RING];
	}
	d->tail += length + 2;
	d->read_count++;
	lck_mtx_unlock(&qmux_lock);
	error = uiomove((caddr_t)message, (int)length, uio);
	kfree_data(message, length);
	return error;
}

static int qmux_message(struct qmux_device *d, const struct hakios_qrtr_ops *ops, uint8_t *message, uint32_t length);

/* one QMUX message a write */
static int
qmux_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct qmux_device *d = qmux_device(dev);
	const struct hakios_qrtr_ops *ops = hakios_mpss_qrtr();
	uint8_t *message;
	user_ssize_t length = uio_resid(uio);
	int error;

	if (d == NULL) {
		return ENXIO;
	}
	if (length < 6 || length > 6 + QMUX_LARGEST || (message = kalloc_data((vm_size_t)length, Z_WAITOK)) == NULL) {
		return EINVAL;
	}
	error = uiomove((caddr_t)message, (int)length, uio);
	if (error == 0) {
		error = qmux_message(d, ops, message, (uint32_t)length);
	}
	kfree_data(message, (vm_size_t)length);
	return error;
}

static int
qmux_message(struct qmux_device *d, const struct hakios_qrtr_ops *ops, uint8_t *message, uint32_t length)
{
	uint32_t whole;

	d->written++;
	if (d->said < 12) {
		d->said++;
		uint8_t b[13] = { 0 };

		memcpy(b, message, length < sizeof(b) ? length : sizeof(b));
		qmux_say("qmux: written %u bytes: %02x %02x%02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x\n", length,
		    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12]);
	}
	whole = (uint32_t)(message[1] | message[2] << 8) + 1;
	if (message[0] != 0x01 || whole > length) {
		return EINVAL;
	}
	if (message[4] == 0) {
		qmux_ctl(d, message + 6, whole - 6);
		return 0;
	}
	lck_mtx_lock(&qmux_lock);
	for (unsigned i = 0; i < QMUX_CLIENTS; i++) {
		struct qmux_client *c = &clients[i];

		if (c->used && c->device == d && c->service == message[4] && c->id == message[5] && c->found) {
			struct hakios_qrtr_port *port = c->port;
			uint32_t node = c->node, to = c->server_port;

			lck_mtx_unlock(&qmux_lock);
			qmux_trace(true, message[4], message[5], message + 6, whole - 6);
			return ops != NULL ? ops->send(port, node, to, message + 6, whole - 6) : ENXIO;
		}
	}
	lck_mtx_unlock(&qmux_lock);
	qmux_say("qmux: a message for service %#x client %u, which is no client of ours\n", message[4], message[5]);
	return 0;
}

static const struct cdevsw qmux_cdevsw = {
	.d_open = qmux_open, .d_close = qmux_close, .d_read = qmux_read, .d_write = qmux_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static thread_call_t start_call;

static void
qmux_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	char name[32];

	qmux_say("qmux: capacity-v2 clients=%u\n", QMUX_CLIENTS);
	qmux_major = cdevsw_add(-1, &qmux_cdevsw);
	for (int unit = 0; unit < QMUX_DEVICES && qmux_major >= 0; unit++) {
		snprintf(name, sizeof(name), "hakios-qmux%d", unit);
		/* (0666: CommCenter runs as _wireless) */
		if (devfs_make_node(makedev(qmux_major, unit), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, name) == NULL) {
			qmux_say("qmux: /dev/%s could not be made\n", name);
		}
	}
}

/* abb=1 only */
__startup_func
static void
qmux_init(void)
{
	uint32_t on = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("abb", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("qmux_wds", &qmux_trace_on, sizeof(qmux_trace_on));
	start_call = thread_call_allocate(qmux_start, NULL);
	clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(start_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, qmux_init);
