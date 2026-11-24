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
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
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
	os_log(OS_LOG_DEFAULT, "hakios: cc: %d of 2 of BootControllerLegacy's slots answer \"booted\" (slide %#lx; slots 2-5 %#lx %#lx %#lx %#lx)",
	    done, (unsigned long)slide, (unsigned long)(vtable[2] - slide), (unsigned long)(vtable[3] - slide),
	    (unsigned long)(vtable[4] - slide), (unsigned long)(vtable[5] - slide));
}
