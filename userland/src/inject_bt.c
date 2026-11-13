/*
 * libhakios.bt.dylib: inside iOS's BlueTool and bluetoothd, for Bluetooth on a controller that is not Broadcom's (the OnePlus
 * 6T's WCN3990, brought up by the kernel: drivers/bluetooth/hakios.bt3990.c and hakios.bthci.c, boot-arg bt=4). Started
 * with them by build.sh BT=1 (INJECT, and the jobs com.apple.bluetoothd, com.apple.BlueTool, com.apple.tccd,
 * com.apple.securityd).
 *
 * Taken from HakiOS's MediaTek branch (oosexgang/hakios-xnu-kernel, branch mtk: userland/src/inject.c, its BlueTool,
 * bluetoothd and os_log parts; docs/bluetooth-mtk-porting.md), where iOS 16.5's bluetoothd reached PoweredOn on the Infinix
 * Hot 20's MT6631. Only what made it work is here, not its diagnostics (bt_diag, the call trace):
 *   BlueTool    iOS's BlueTool brings a Broadcom chip up before bluetoothd opens it (a boot script, a firmware patch, the
 *               UART's speed); this chip is brought up by the kernel. So in this process the service com.apple.BlueTool
 *               is ours, and bluetoothd's "boot" and "init" are answered as done. It has to be the process launchd started:
 *               a program of ours is not a platform binary, and launchd panics the kernel at the exec of one.
 *   bluetoothd  one change, at bluetoothd+0x58e798 of iOS 16.5 (20F66), the constructor of a scan entry: an application
 *               without a foreground state, or without a list of services, gets the scan results all the same (otherwise
 *               a scan runs and delivers nothing to it). The 16 bytes there are compared with what they were when this was
 *               written; on any other bluetoothd nothing is changed and that is said.
 *   its log     what bluetoothd says of its state (its os_log calls: the stack's start, power, transport, vendor commands,
 *               "Bluetooth error - restarting" with its reason) goes to the kernel's log through sysctl kern.hakios.bt_say,
 *               at most bt_lines=N lines (60); the console loses a daemon's lines when the daemon goes.
 */
#include <dispatch/dispatch.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

static void
bt_say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		(void)sysctlbyname("kern.hakios.bt_say", NULL, NULL, line, (size_t)n);
	}
}

static bool in_bluetoothd;
static unsigned may_say = 60, said;

/* ---- BlueTool ---- */
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, void (^handler)(xpc_object_t object));
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern const struct _xpc_type_s *xpc_get_type(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern void xpc_dictionary_set_int64(xpc_object_t xdict, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
extern char *xpc_copy_description(xpc_object_t object);
extern void xpc_release(xpc_object_t object);

static void
bluetool_serves(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", NULL, 1 /* LISTENER */);

	bt_say("BlueTool[%d]: com.apple.BlueTool is ours (the kernel has the controller on)", getpid());
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) != &_xpc_type_dictionary) {
				return;
			}
			char *text = xpc_copy_description(message);

			bt_say("BlueTool: asked %.200s", text != NULL ? text : "(?)");
			free(text);
			xpc_object_t reply = xpc_dictionary_create_reply(message);

			if (reply != NULL) {
				xpc_dictionary_set_int64(reply, "result", 0);
				xpc_dictionary_set_int64(reply, "manufacturer", 15);    /* Broadcom's company identifier, as the MediaTek branch answered (what bluetoothd expects of BlueTool) */
				xpc_dictionary_set_string(reply, "firmware", "WCN3990");
				xpc_dictionary_set_string(reply, "output", "");
				xpc_connection_send_message(peer, reply);
				xpc_release(reply);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	dispatch_main();
}

/* ---- bluetoothd: the scan entry ---- */
__attribute__((visibility("hidden"), used)) uintptr_t bt_entry_cont;

/*
 * What a scan entry is made of (bluetoothd of iOS 16.5, the MediaTek branch's finding, runs 676-711). CoreBluetooth gives an
 * application scan results only if the entry made for its scan passes bluetoothd+0x58f2a0, which for the entry type 1 (an
 * application) wants the session in the foreground (the third argument of the entry's constructor) and a list of services to
 * scan for (config +8..+0x10). So: every entry is made for a session in the foreground (the stub), and an entry with no
 * services to look for, which is what scanForPeripherals(withServices: nil) is, becomes one of the type 0x20, the kind the
 * daemon's own scans are: it looks at everything (config byte +1). Returns the type the entry is to have.
 */
__attribute__((visibility("hidden"), used, noinline)) uint64_t
bt_entry_fix(uint64_t type, uint8_t *config)
{
	if (type == 1 && (uintptr_t)config > 0x100000000UL && *(const uint64_t *)(config + 0x08) == *(const uint64_t *)(config + 0x10)) {
		config[1] = 1;
		return 0x20;
	}
	return type;
}

/* the constructor's first four instructions, displaced here, then its fifth on (x0 this, x1 config, x2 foreground, x3 type) */
extern const uint32_t bt_entry_displaced[];
__attribute__((naked, used)) static void
bt_entry_stub(void)
{
	__asm__ volatile (
		"sub sp, sp, #0xa0\n"
		"stp x0, x1, [sp]\n" "stp x2, x3, [sp, #16]\n" "stp x4, x5, [sp, #32]\n" "stp x6, x7, [sp, #48]\n" "stp x8, x9, [sp, #64]\n"
		"stp x29, x30, [sp, #80]\n" "stp q0, q1, [sp, #96]\n" "stp q2, q3, [sp, #128]\n"
		"ldr x0, [sp, #24]\n" "ldr x1, [sp, #8]\n"
		"bl _bt_entry_fix\n"
		"str x0, [sp, #24]\n"
		"ldp x0, x1, [sp]\n" "ldp x2, x3, [sp, #16]\n" "ldp x4, x5, [sp, #32]\n" "ldp x6, x7, [sp, #48]\n" "ldp x8, x9, [sp, #64]\n"
		"ldp x29, x30, [sp, #80]\n" "ldp q0, q1, [sp, #96]\n" "ldp q2, q3, [sp, #128]\n"
		"add sp, sp, #0xa0\n"
		".private_extern _bt_entry_displaced\n"
		"_bt_entry_displaced:\n"
		"sub sp, sp, #0xb0\n" "stp x28, x27, [sp, #0x50]\n" "stp x26, x25, [sp, #0x60]\n" "stp x24, x23, [sp, #0x70]\n"
		"mov x2, #1\n"
		"adrp x16, _bt_entry_cont@PAGE\n" "ldr x16, [x16, _bt_entry_cont@PAGEOFF]\n" "br x16\n"
	);
}

static void
bt_entry_install(uintptr_t base)
{
	uint32_t *at = (uint32_t *)(base + (0x10058e798UL - 0x100000000UL));
	vm_address_t page = (vm_address_t)at & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;

	if ((vm_address_t)at + 16 > page + vm_page_size) {
		bt_say("bluetoothd: +0x58e798 crosses a page; left alone");
	} else if (memcmp(at, bt_entry_displaced, 16) != 0) {
		bt_say("bluetoothd: +0x58e798 holds %08x %08x %08x %08x, not iOS 16.5's: left alone (an app's scan may deliver nothing)",
		    at[0], at[1], at[2], at[3]);
	} else if ((kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) != KERN_SUCCESS) {
		bt_say("bluetoothd: cannot write its code (%d)", kr);
	} else {
		uint32_t jump[4] = { 0x58000050 /* ldr x16, #8 */, 0xd61f0200 /* br x16 */, (uint32_t)(uintptr_t)bt_entry_stub,
		    (uint32_t)((uintptr_t)bt_entry_stub >> 32) };

		bt_entry_cont = (uintptr_t)at + 16;
		memcpy(at, jump, 16);
		kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
		sys_icache_invalidate(at, 16);
		bt_say("bluetoothd: the scan entry's constructor (+0x58e798) is ours (%d)", kr);
	}
}

/* ---- bluetoothd: what it says of its state ---- */
extern void _os_log_impl(void *dso, void *log, int type, const char *format, uint8_t *buf, uint32_t size);
extern void _os_log_error_impl(void *dso, void *log, int type, const char *format, uint8_t *buf, uint32_t size);
extern void _os_log_fault_impl(void *dso, void *log, int type, const char *format, uint8_t *buf, uint32_t size);

static void
os_log_watched(const char *format, const uint8_t *buf, uint32_t size)
{
	char text[200] = "";
	const uint8_t *at = buf + 2, *end = buf + size;
	unsigned arg = 0, count = buf[1];
	bool restart = strstr(format, "Bluetooth error") != NULL;
	bool plain = strcmp(format, "%{public}s") == 0 || strcmp(format, "%s") == 0;
	bool wanted = restart || plain || strstr(format, "ower") != NULL || strstr(format, "tate") != NULL ||
	    strstr(format, "VSC") != NULL || strstr(format, "stall") != NULL || strstr(format, "ompleted") != NULL ||
	    strstr(format, "eady") != NULL || strstr(format, "ransport") != NULL || strstr(format, "tack") != NULL ||
	    strstr(format, "hipset") != NULL || strstr(format, "ailed") != NULL || strstr(format, "No bluetooth") != NULL ||
	    strstr(format, "ddress") != NULL || strstr(format, "air") != NULL || strstr(format, "eychain") != NULL ||
	    strstr(format, "HID") != NULL || strstr(format, "onnect") != NULL || strstr(format, "ond") != NULL;
	/* (the 6T, runs 89-92: these, many times a second, used up bt_lines before anything was paired) */
	bool noise = strstr(format, "decryptProximityPairingPayload") != NULL || strstr(format, "statedump") != NULL ||
	    strstr(format, "getDeviceEncryptionKey") != NULL || strstr(format, "Unable to locate device address") != NULL ||
	    strstr(format, "advertisingAddressDidChange") != NULL || strstr(format, "Power state changed") != NULL ||
	    strstr(format, "PowerManager power state") != NULL || strstr(format, "canScanNow") != NULL ||
	    strstr(format, "ShouldScan") != NULL || strstr(format, "AnyValidScanRequests") != NULL;

	if (!wanted || noise || said >= may_say) {
		return;
	}
	/* the arguments, in the order of the format's conversions: s a string, d/u/x... a number, the rest skipped */
	for (const char *f = format; *f != 0 && arg < count && at + 2 <= end; f++) {
		char conversion;
		uint8_t length;

		if (*f != '%') {
			continue;
		}
		f++;
		if (*f == '%') {
			continue;
		}
		if (*f == '{') {
			while (*f != 0 && *f != '}') {
				f++;
			}
			if (*f == '}') {
				f++;
			}
		}
		while (*f != 0 && strchr("diouxXcsSpf@P", *f) == NULL) {
			f++;
		}
		conversion = *f;
		if (conversion == 0) {
			break;
		}
		length = at[1];
		if (at + 2 + length > end) {
			break;
		}
		if (conversion == 's' && length == 8) {
			const char *string;

			memcpy(&string, at + 2, 8);
			if (string != NULL && (uintptr_t)string > 0x100000000UL) {
				snprintf(text + strlen(text), sizeof(text) - strlen(text), " \"%.120s\"", string);
			}
		} else if (strchr("diouxXc", conversion) != NULL && (length == 4 || length == 8 || length == 1 || length == 2)) {
			long long number = 0;

			memcpy(&number, at + 2, length);
			snprintf(text + strlen(text), sizeof(text) - strlen(text), " %lld", number);
		}
		at += 2 + length;
		arg++;
	}
	if (plain && strstr(text, "Bluetooth error") == NULL && strstr(text, "stall") == NULL && strstr(text, "ailed") == NULL) {
		return;
	}
	said++;
	bt_say("bt[%d] %.150s%s", getpid(), format, text);
}

#define OS_LOG_WATCHED(name)                                                                                   \
static void                                                                                                    \
name##_watched(void *dso, void *log, int type, const char *format, uint8_t *buf, uint32_t size)               \
{                                                                                                              \
	if (in_bluetoothd && format != NULL && buf != NULL && size >= 2) {                                     \
		os_log_watched(format, buf, size);                                                             \
	}                                                                                                      \
	name(dso, log, type, format, buf, size);                                                               \
}
OS_LOG_WATCHED(_os_log_impl)
OS_LOG_WATCHED(_os_log_error_impl)
OS_LOG_WATCHED(_os_log_fault_impl)

__attribute__((used)) static const struct {
	const void *replacement;
	const void *replacee;
} os_log_interpose[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)_os_log_impl_watched, (const void *)_os_log_impl },
	{ (const void *)_os_log_error_impl_watched, (const void *)_os_log_error_impl },
	{ (const void *)_os_log_fault_impl_watched, (const void *)_os_log_fault_impl },
};

static void
bluetoothd_exits(void)
{
	bt_say("bluetoothd[%d] exits", getpid());
}

__attribute__((constructor))
static void
hakios_bt_init(void)
{
	const char *name = getprogname();

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "BlueTool") == 0) {
		bluetool_serves();              /* does not return */
	} else if (strcmp(name, "bluetoothd") == 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "bt_lines=") != NULL) {
			may_say = (unsigned)atoi(strstr(arguments, "bt_lines=") + 9);
		}
		in_bluetoothd = true;
		atexit(bluetoothd_exits);
		bt_say("bluetoothd[%d] starts", getpid());
		for (uint32_t image = 0; image < _dyld_image_count(); image++) {
			const char *path = _dyld_get_image_name(image);
			size_t length = path != NULL ? strlen(path) : 0;

			if (length >= 11 && strcmp(path + length - 11, "/bluetoothd") == 0) {
				bt_entry_install((uintptr_t)_dyld_get_image_header(image));
			}
		}
	}
}
