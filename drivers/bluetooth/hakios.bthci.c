/*
 * Bluetooth for iOS's own bluetoothd: the half that does not depend on the chip. What iOS 16.5's bluetoothd needs from a
 * kernel to take a controller as its own -- the kernel control com.apple.uart.sk.bluetooth and the Skywalk kernel pipe
 * behind it (hakios.btsk.c), the nodes it opens, the service "bluetooth" in the registry (hakios.bluetooth.cpp), H4
 * framing of what it writes, and the answers Apple's stack does not take from a controller that is not Broadcom's. A chip
 * driver (hakios.bt3990.c, the OnePlus 6T's WCN3990) brings its controller up to where it takes HCI and then calls
 * hakios_bthci_start() with three operations: send one H4 packet, receive one H4 packet, switch it off.
 *
 * Taken from HakiOS's MediaTek branch (oosexgang/hakios-xnu-kernel, branch mtk: xnu/files/bsd/dev/redwood_mtkbt_skel.c,
 * and docs/bluetooth-mtk-porting.md, which says how it was found on the Infinix Hot 20's MT6631, run by run). What is
 * different here:
 *   - Apple's vendor commands (OGF 0x3f) are Broadcom's, and a Qualcomm controller has vendor commands of its own in the
 *     same space (0xfc00 is its patch download, 0xfc0b its NVM, 0xfc14 its address): one of Broadcom's could be one of
 *     Qualcomm's. So none of them is sent to the controller. Each is answered here, as the MediaTek branch learned
 *     bluetoothd wants it answered: Command Complete, status 0, and the data its parser reads after the status -- one
 *     byte for 0xfd48, 16 for 0xfce2 with sub-command 0x15 or 0x36, 64 (the first 0x51) for 0xfd1b, none for the rest.
 *   - the controller's own vendor events (event 0xff: Qualcomm's logs and the like) are not given to bluetoothd.
 *   - the address is the chip driver's (it writes the same one into the controller), not the Wi-Fi radio's plus one.
 * Kept as they were: Write_Default_Erroneous_Data_Reporting (0x0c5b) answered as done when the controller says it does not
 * know it, and Read_Local_Supported_Commands with the LE commands of octets 24-28 OR-ed in.
 *
 * Also here: sysctl kern.hakios.bt_say, a line from user space (the library inside bluetoothd and BlueTool, userland/src/
 * inject.c) put into the kernel's log, where the run's log has it.
 *
 * Boot-args: bt_said=N   packets of each direction said in the log (96)
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/sys_domain.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <uuid/uuid.h>

extern void loader_log_putc(char c);
extern bool HakiosBluetoothNode(void);                  /* hakios.bluetooth.cpp: the service "bluetooth" */
extern void HakiosBluetoothAddress(const uint8_t *bt);  /* its local-mac-address */
extern void hakios_btsk_init(void);                     /* hakios.btsk.c: the Skywalk kernel pipe bluetoothd's packets go through */
extern bool hakios_btsk_uuid(uuid_t out);
extern void hakios_btsk_deliver(const uint8_t *bytes, unsigned length);

void hakios_bt_say(const char *format, ...) __printflike(1, 2);
void hakios_bthci_from_host(const uint8_t *bytes, unsigned length);
bool hakios_bthci_start(const char *name, int (*send)(const uint8_t *, unsigned), int (*receive)(uint8_t *, unsigned, unsigned),
    void (*off)(void), const uint8_t *address, bool publish);

SYSCTL_DECL(_kern_hakios);

#define BTHCI_TXQ               8192u           /* what bluetoothd has written and the controller has not been given yet */
#define BTHCI_FRAME             4096u           /* one packet from the controller */
#define BTHCI_ANSWER_MS         2000u           /* a command of the probe is given this long */

void
hakios_bt_say(const char *format, ...)
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

#define say(...) hakios_bt_say("bthci: " __VA_ARGS__)

/* a line from user space for the log (userland/src/inject.c: bluetoothd's and BlueTool's) */
static int
bt_say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static uint32_t lines;
	char line[256];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 2) {
		length = sizeof(line) - 2;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == 0)) {
		length--;
	}
	line[length] = 0;
	if (lines++ < 60000) {          /* (run 89: 2000 were gone before the first pairing) */
		hakios_bt_say("bt-user: %s\n", line);
	}
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, bt_say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    bt_say_line, "A", "a line of Bluetooth's user space for the kernel's log");

static uint32_t
ms_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/* ---- the chip's operations ---- */
static struct {
	const char *name;
	int (*send)(const uint8_t *h4, unsigned length);
	int (*receive)(uint8_t *h4, unsigned room, unsigned wait_ms);
	void (*off)(void);
} chip;

static uint32_t said_max = 96;

/* ---- H4 ---- */

/* the length of the H4 packet that begins the bytes; 0 while its header is not whole or the first byte is not a packet type */
static unsigned
h4_length(const uint8_t *b, unsigned have)
{
	if (have < 1) {
		return 0;
	}
	switch (b[0]) {
	case 1:                                 /* command: opcode (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 2:                                 /* ACL: handle (2), length (2) */
		return have >= 5 ? 5u + (b[3] | ((unsigned)b[4] << 8)) : 0;
	case 3:                                 /* SCO: handle (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 4:                                 /* event: code (1), length (1) */
		return have >= 3 ? 3u + b[2] : 0;
	case 5:                                 /* ISO: handle (2), length (2, 14 bits) */
		return have >= 5 ? 5u + ((b[3] | ((unsigned)b[4] << 8)) & 0x3fff) : 0;
	default:
		return 0;
	}
}

static void
dump(const char *direction, const uint8_t *bytes, unsigned length, uint32_t *counter)
{
	char text[3 * 32 + 1];
	unsigned n = length < 32 ? length : 32;

	if ((*counter)++ >= said_max) {
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		snprintf(text + 3 * i, 4, " %02x", bytes[i]);
	}
	text[3 * n] = 0;
	say("%s %u bytes:%s%s\n", direction, length, text, length > n ? " ..." : "");
}

/* ---- what bluetoothd writes: one queue, drained a whole packet at a time by the bridge ---- */
static LCK_GRP_DECLARE(bthci_lck_grp, "hakios-bthci");
static LCK_MTX_DECLARE(txq_lock, &bthci_lck_grp);
static uint8_t txq[BTHCI_TXQ];
static unsigned txq_count;
static uint32_t txq_dropped;                    /* bytes that found the queue full */

static bool
queue_locked(const uint8_t *bytes, size_t length)
{
	if (length > BTHCI_TXQ - txq_count) {
		txq_dropped += (uint32_t)length;
		return false;
	}
	memcpy(txq + txq_count, bytes, length);
	txq_count += (unsigned)length;
	return true;
}

/* H4 bytes the Skywalk channel's user wrote (hakios.btsk.c's sync_tx) */
void
hakios_bthci_from_host(const uint8_t *bytes, unsigned length)
{
	lck_mtx_lock(&txq_lock);
	(void)queue_locked(bytes, length);
	lck_mtx_unlock(&txq_lock);
}

/*
 * ---- answers bluetoothd does not take as a controller gives them ----
 */
static uint8_t frame[BTHCI_FRAME];
static uint8_t packet[BTHCI_TXQ];
static uint32_t said_in, said_out, said_vsc, vse_dropped;

/* one of Apple's (Broadcom's) vendor commands, answered here as done (see the head of the file); the answer's length */
static unsigned
vendor_answer(const uint8_t *command, unsigned length, uint8_t *answer)
{
	const uint8_t lo = command[1], hi = command[2];
	const uint8_t sub = length >= 5 ? command[4] : 0;
	const bool antenna = lo == 0xe2 && hi == 0xfc && (sub == 0x15 || sub == 0x36);
	const bool cal = lo == 0x1b && hi == 0xfd;
	const unsigned extra = cal ? 64 : antenna ? 16 : (lo == 0x48 && hi == 0xfd) ? 1 : 0;

	answer[0] = 0x04;
	answer[1] = 0x0e;
	answer[2] = (uint8_t)(4 + extra);
	answer[3] = 0x01;
	answer[4] = lo;
	answer[5] = hi;
	answer[6] = 0x00;
	for (unsigned i = 0; i < extra; i++) {
		answer[7 + i] = 0x00;
	}
	if (cal) {
		answer[7] = 0x51;                       /* the table's number (the MediaTek branch's guess, which bluetoothd took) */
	}
	if (said_vsc++ < 60) {
		say("vendor command %02x%02x (%u bytes%s%02x) answered here as done, %u bytes of data\n", hi, lo, length,
		    length >= 5 ? ", first parameter " : "", length >= 5 ? sub : 0, extra);
	}
	return 7 + extra;
}

static bool
bt_extadv_kept(void)
{
	uint32_t keep = 0;

	PE_parse_boot_argn("bt_extadv", &keep, sizeof(keep));
	return keep != 0;
}

/* the controller's answer, changed where Apple's stack wants it; its new length, 0 to give bluetoothd nothing */
static unsigned
quirks(uint8_t *p, unsigned length)
{
	static const uint8_t le_commands[4] = { 0xf7, 0xff, 0xff, 0x7f };

	/* Qualcomm's controller logs, as ACL data on handle 0x2edc (Linux's hci_qca: QCA_DEBUG_HANDLE): not bluetoothd's */
	if (length >= 5 && p[0] == 0x02 && p[1] == 0xdc && (p[2] & 0x0f) == 0x0e) {
		static unsigned diag_dropped;

		if (diag_dropped++ < 4) {
			say("the controller's own log (ACL on handle 0x2edc, %u bytes): not given to bluetoothd\n", length);
		}
		return 0;
	}
	if (length >= 3 && p[0] == 0x04 && p[1] == 0xff) {
		if (vse_dropped++ < 8) {
			say("the controller's own vendor event (%u bytes, %02x %02x ...): not given to bluetoothd\n", length,
			    length > 3 ? p[3] : 0, length > 4 ? p[4] : 0);
		}
		return 0;
	}
	/* LE Read Local Supported Features (0x2003): extended (bit 12) and periodic (bit 13) advertising taken out, as above */
	if (length >= 15 && p[0] == 0x04 && p[1] == 0x0e && p[4] == 0x03 && p[5] == 0x20 && p[6] == 0x00 && !bt_extadv_kept()) {
		p[7 + 1] &= (uint8_t)~0x30;
	}
	/* Write_Default_Erroneous_Data_Reporting: "Unknown HCI Command", as Command Status or as Command Complete */
	if (length == 7 && memcmp(p, "\x04\x0f\x04\x01\x01\x5b\x0c", 7) == 0) {
		memcpy(p, "\x04\x0e\x04\x01\x5b\x0c\x00", 7);
		return 7;
	}
	if (length == 7 && memcmp(p, "\x04\x0e\x04\x01\x5b\x0c\x01", 7) == 0) {
		p[6] = 0x00;
		return 7;
	}
	if (length >= 7 + 64 && p[0] == 0x04 && p[1] == 0x0e && p[2] >= 0x44 && p[4] == 0x02 && p[5] == 0x10 && p[6] == 0x00) {
		for (unsigned i = 0; i < 4; i++) {
			p[7 + 25 + i] |= le_commands[i];
		}
		p[7 + 24] |= 0x60;
		/*
		 * The WCN3990 (HCI 5.0), runs 86 and 87: bluetoothd's next command after 0x0c31 is 0x0c5b, whose bits (octet 18,
		 * 2 and 3) this controller leaves clear: "HCI initialization failed". They are set (0x0c5b is answered as done
		 * below if the controller does not know it). And with LE extended advertising and scanning in the mask bluetoothd
		 * uses them and then the legacy ones too, which this controller refuses once the extended ones are used (0x2007,
		 * 0x200b: "Command Disallowed"; "Read LE Advertising TX power failed", exit(1)): the extended and periodic
		 * advertising and the extended scanning and connecting are taken out (octet 36 bits 2-7, 37, 38), as from a
		 * controller of 4.2's kind. bt_extadv=1 leaves them.
		 */
		p[7 + 18] |= 0x0c;
		if (!bt_extadv_kept()) {
			p[7 + 36] &= 0x03;
			p[7 + 37] = 0;
			p[7 + 38] = 0;
		}
		/* bt_allcmds=1 (a look): every command said supported, so that bluetoothd sends the one it wanted next and the log
		 * names it (run 86: "HCI initialization failed" right after 0x0c31, nothing sent -- it looks in the mask first) */
		{
			uint32_t all = 0;
			static unsigned said_mask;

			PE_parse_boot_argn("bt_allcmds", &all, sizeof(all));
			if (said_mask++ < 2) {
				char hex[3 * 64 + 1];
				unsigned n = 0;

				for (unsigned i = 0; i < 64; i++) {
					n += (unsigned)snprintf(hex + n, sizeof(hex) - n, " %02x", p[7 + i]);
				}
				say("supported commands (octets 0..63, after the LE bits%s):%s\n", all ? ", before all set" : "", hex);
			}
			if (all) {
				memset(p + 7, 0xff, 64);
			}
		}
	}
	return length;
}

/* ---- the kernel control ---- */
static kern_ctl_ref ctl_ref;

static errno_t
ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	say("control connected by %s (pid %d): unit %u\n", name, proc_selfpid(), sac->sc_unit);
	return 0;
}

static errno_t
ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	say("control: unit %u disconnected\n", unit);
	return 0;
}

static errno_t
ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	errno_t error = 0;

	lck_mtx_lock(&txq_lock);
	for (mbuf_t part = m; part != NULL; part = mbuf_next(part)) {
		if (!queue_locked(mbuf_data(part), mbuf_len(part))) {
			error = ENOBUFS;
			break;
		}
	}
	lck_mtx_unlock(&txq_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, __unused void *data, size_t len)
{
	say("control: setsockopt %#x, %zu bytes: taken, nothing done\n", (unsigned)opt, len);
	return 0;
}

static errno_t
ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	uuid_t uuid;
	bool have = (unsigned)opt == 0x40105412u && data != NULL && *len >= sizeof(uuid_t) && hakios_btsk_uuid(uuid);

	say("control: getsockopt %#x, room for %zu bytes: %s\n", (unsigned)opt, *len, have ? "the channel's UUID" : "nothing");
	if (have) {
		memcpy(data, uuid, sizeof(uuid_t));
		*len = sizeof(uuid_t);
	} else {
		*len = 0;
	}
	return 0;
}

static bool
bt_ctl_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;       /* (a datagram socket: "Protocol wrong type for socket") */
	reg.ctl_sendsize = 65536;
	reg.ctl_recvsize = 65536;
	reg.ctl_connect = ctl_connect;
	reg.ctl_disconnect = ctl_disconnect;
	reg.ctl_send = ctl_send;
	reg.ctl_setopt = ctl_setopt;
	reg.ctl_getopt = ctl_getopt;
	error = ctl_register(&reg, &ctl_ref);
	say("kernel control com.apple.uart.sk.bluetooth %s (%d)\n", error == 0 ? "registered" : "NOT REGISTERED", error);
	return error == 0;
}

/*
 * ---- the nodes ----
 * /dev/btwake, /dev/btreset, /dev/btpoweroff: bluetoothd opens btwake again and again, BlueTool the others; on Apple's
 * hardware they are pins. Nothing is behind them here: opened, read as empty, written and forgotten, the terminal ioctls taken.
 */
static int
node_open(dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
	static uint32_t said;
	char name[32] = "";

	if (said < 8) {
		said++;
		proc_selfname(name, sizeof(name));
		say("node %u opened by %s (pid %d) with flags %#x\n", minor(dev), name, proc_selfpid(), flags);
	}
	return 0;
}

static int
node_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
node_read(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return 0;
}

static int
node_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint32_t said;
	uint8_t first = 0;
	user_ssize_t n = uio_resid(uio);

	if (n > 0 && uiomove((caddr_t)&first, 1, uio) == 0 && said < 8) {
		said++;
		say("node %u written %lld byte(s), the first %02x: nothing is behind it\n", minor(dev), (long long)n, first);
	}
	uio_setresid(uio, 0);
	return 0;
}

static int
node_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	static u_long said[16];
	static unsigned said_count;
	bool seen = false;
	int result = 0;

	switch (cmd) {
	case TIOCGETA: {
		struct termios *t = (struct termios *)(void *)data;

		bzero(t, sizeof(*t));
		t->c_cflag = CS8 | CREAD | CLOCAL | HUPCL;
		t->c_ispeed = t->c_ospeed = 3000000;
		break;
	}
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF: case TIOCEXCL: case TIOCNXCL: case TIOCSDTR: case TIOCCDTR:
	case TIOCMBIS: case TIOCMBIC: case TIOCMSET: case TIOCFLUSH: case TIOCSBRK: case TIOCCBRK: case TIOCDRAIN:
	case FIONBIO: case FIOASYNC:
		break;
	case TIOCMGET:
		*(int *)(void *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RTS | TIOCM_DTR;
		break;
	case FIONREAD:
		*(int *)(void *)data = 0;
		break;
	default:
		result = ENOTTY;
		break;
	}
	for (unsigned i = 0; i < said_count; i++) {
		seen |= said[i] == cmd;
	}
	if (!seen && said_count < sizeof(said) / sizeof(said[0])) {
		said[said_count++] = cmd;
		say("node %u: ioctl %#lx: %s\n", minor(dev), cmd, result == 0 ? "taken" : "not a command of this node (ENOTTY)");
	}
	return result;
}

static int
node_select(__unused dev_t dev, __unused int which, __unused void *wql, __unused struct proc *p)
{
	return 1;
}

static const struct cdevsw node_cdevsw = {
	.d_open = node_open, .d_close = node_close, .d_read = node_read, .d_write = node_write,
	.d_ioctl = node_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = node_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
make_nodes(void)
{
	static const char *const names[] = { "btwake", "btreset", "btpoweroff" };
	int major = cdevsw_add(-1, &node_cdevsw);
	bool made = major >= 0;

	for (unsigned i = 0; made && i < sizeof(names) / sizeof(names[0]); i++) {
		made = devfs_make_node(makedev(major, i), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "%s", names[i]) != NULL;
	}
	say("/dev/btwake, /dev/btreset, /dev/btpoweroff %s (major %d)\n", made ? "made" : "NOT MADE", major);
	return made;
}

/* ---- the controller ---- */

/* One packet from the controller, checked: its length is the one its own header says. 0 if none, less than 0 for an error. */
static int
receive(unsigned wait_ms)
{
	static uint32_t said_bad;
	int length = chip.receive(frame, BTHCI_FRAME - 80, wait_ms);     /* (80: room for what quirks adds) */

	if (length <= 0) {
		return length;
	}
	if ((unsigned)length > BTHCI_FRAME - 80 || h4_length(frame, (unsigned)length) != (unsigned)length) {
		if (said_bad < 8) {
			said_bad++;
			say("the chip gave %d bytes that are not one H4 packet (type %02x): dropped\n", length, frame[0]);
		}
		return 0;
	}
	return length;
}

/* A command of the probe: sent, and answered by a Command Complete of its opcode with status 0 within BTHCI_ANSWER_MS. */
static bool
probe_command(const char *what, uint16_t opcode)
{
	const uint8_t command[4] = { 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), 0x00 };
	uint32_t since = ms_now();
	int error, length;

	dump("out", command, sizeof(command), &said_out);
	if ((error = chip.send(command, sizeof(command))) != 0) {
		say("%s: send failed (%d)\n", what, error);
		return false;
	}
	while (ms_now() - since < BTHCI_ANSWER_MS) {
		if ((length = receive(10)) < 0) {
			say("%s: receive failed (%d)\n", what, length);
			return false;
		}
		if (length == 0) {
			continue;
		}
		dump("in ", frame, (unsigned)length, &said_in);
		if (length >= 7 && frame[0] == 0x04 && frame[1] == 0x0e && frame[4] == command[1] && frame[5] == command[2]) {
			if (frame[6] != 0) {
				say("%s: the controller's status is %#x\n", what, frame[6]);
			}
			return frame[6] == 0;
		}
	}
	say("%s: no Command Complete in %u ms\n", what, BTHCI_ANSWER_MS);
	return false;
}

/* what is written is sent a packet at a time; what comes is given to the Skywalk channel. Never returns. */
static void
bridge(void)
{
	uint32_t pending_since = 0, errors = 0;
	bool said_pending = false;

	for (;;) {
		int length;

		for (;;) {
			unsigned n, have;
			int error;

			lck_mtx_lock(&txq_lock);
			have = txq_count;
			if (have > 0 && (txq[0] < 1 || txq[0] > 5 || txq[0] == 4)) {
				/* not a packet the host sends: the stream has lost its framing and nothing after it can be trusted */
				say("%u bytes written begin with %02x, not an H4 packet type: all dropped\n", have, txq[0]);
				txq_count = 0;
				lck_mtx_unlock(&txq_lock);
				break;
			}
			n = h4_length(txq, have);
			if (n == 0 || n > have) {
				if (n > BTHCI_TXQ) {
					say("a packet of %u bytes is longer than the queue (%u): %u bytes dropped\n", n, BTHCI_TXQ, have);
					txq_count = 0;
				}
				lck_mtx_unlock(&txq_lock);
				break;
			}
			memcpy(packet, txq, n);
			memmove(txq, txq + n, have - n);
			txq_count = have - n;
			lck_mtx_unlock(&txq_lock);
			dump("out", packet, n, &said_out);
			if (packet[0] == 0x01 && n >= 4 && (packet[2] >> 2) == 0x3f) {
				uint8_t answer[80];
				unsigned a = vendor_answer(packet, n, answer);

				dump("in ", answer, a, &said_in);
				hakios_btsk_deliver(answer, a);
				continue;
			}
			if ((error = chip.send(packet, n)) != 0 && errors++ < 8) {
				say("send of %u bytes failed (%d)\n", n, error);
			}
		}
		{
			/* bytes written and not a whole packet for 2 s: said once (a writer that stopped half-way, or a length this cannot frame) */
			unsigned pending;
			uint32_t dropped;

			lck_mtx_lock(&txq_lock);
			pending = txq_count;
			dropped = txq_dropped;
			txq_dropped = 0;
			lck_mtx_unlock(&txq_lock);
			if (dropped != 0) {
				say("the queue was full: %u bytes from bluetoothd were dropped\n", dropped);
			}
			if (pending == 0) {
				pending_since = 0;
			} else if (pending_since == 0) {
				pending_since = ms_now() | 1;
			} else if (!said_pending && ms_now() - pending_since > 2000) {
				said_pending = true;
				say("%u bytes written and not a whole H4 packet in 2 s\n", pending);
			}
		}
		length = receive(2);
		if (length > 0) {
			length = (int)quirks(frame, (unsigned)length);
			if (length > 0) {
				dump("in ", frame, (unsigned)length, &said_in);
				hakios_btsk_deliver(frame, (unsigned)length);
			}
		} else if (length < 0) {
			if (errors++ < 8) {
				say("receive failed (%d)\n", length);
			}
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
}

/*
 * The controller, asked what a host asks first; then, if `publish`, what iOS looks for is made and the bridge runs (this
 * does not return). Without `publish` it returns whether all six were answered, and the controller is left on.
 * `address` is the Bluetooth address in the order it is written (aa:bb:cc:dd:ee:ff is aa first), or NULL.
 */
bool
hakios_bthci_start(const char *name, int (*send)(const uint8_t *, unsigned), int (*receive_op)(uint8_t *, unsigned, unsigned),
    void (*off)(void), const uint8_t *address, bool publish)
{
	static const struct {
		const char *what;
		uint16_t opcode;
	} probe[] = {
		{ "HCI_Reset", 0x0c03 },
		{ "HCI Read_Local_Version_Information", 0x1001 },
		{ "HCI Read_BD_ADDR", 0x1009 },
		{ "HCI Read_Local_Supported_Features", 0x1003 },
		{ "HCI Read_Buffer_Size", 0x1005 },
		{ "HCI LE_Read_Buffer_Size", 0x2002 },
	};
	unsigned good = 0;
	uint32_t value;

	if (PE_parse_boot_argn("bt_said", &value, sizeof(value))) {
		said_max = value;
	}
	chip.name = name;
	chip.send = send;
	chip.receive = receive_op;
	chip.off = off;
	for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		good += probe_command(probe[i].what, probe[i].opcode);
	}
	say("%s: %u of %u HCI commands answered as a controller answers\n", name, good, (unsigned)(sizeof(probe) / sizeof(probe[0])));
	if (good != sizeof(probe) / sizeof(probe[0])) {
		say("%s: not a controller iOS can be given\n", name);
		return false;
	}
	if (!publish) {
		return true;
	}
	if (address != NULL) {
		HakiosBluetoothAddress(address);
	} else {
		say("%s: no address: bluetoothd will say \"No bluetooth on this device.\"\n", name);
	}
	/* iOS looks for the controller in the registry ("bluetooth"): without it BluetoothManager's attach fails with 9 and Settings' switch stays grey */
	{
		bool node = false;

		for (unsigned tries = 0; tries < 40 && !(node = HakiosBluetoothNode()); tries++) {
			delay_for_interval(250, NSEC_PER_MSEC);
		}
		say("the registry's \"bluetooth\" service %s\n", node ? "made" : "could not be made in 10 s: Settings will not attach");
	}
	if (!make_nodes() || !bt_ctl_register()) {
		return false;
	}
	hakios_btsk_init();
	say("%s: the bridge runs: bluetoothd may come\n", name);
	bridge();
	return true;
}
