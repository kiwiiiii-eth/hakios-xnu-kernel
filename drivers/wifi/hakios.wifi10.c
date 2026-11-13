/*
 * Darwin-on-hakios: the OnePlus 6T's Wi-Fi, the WCN3990 of the SDM845, with Linux's ath10k (driver WIFI10).
 *
 * The radio's firmware (wlanmdsp.mbn) runs on the modem, which drivers/modem/hakios.mpss845.c starts and links
 * (boot-args modem_step=10 modem_keep=1: the modem up, GLINK and QRTR up, kept so while the system runs). What
 * hakios.wifi.c is to the reference phone's WCN6750 this file is to the WCN3990: the phone's side of the driver's
 * object (tools/mk-ath10k.sh, wifi/lx/src10) that hakios.lx.c asks for -- the firmware's files, the radio's view of
 * memory (its SMMU stream, through hakios_mpss_wifi_space()), its interrupts (wired: GIC SPIs 414..425, levels),
 * the hypervisor's MP_ASSIGN for the MSA (ath10k's qmi.c), the network to join -- and its start: with wifi=1, once the
 * modem's QRTR is up, the QMI helpers are put on it and the driver is started (hakios_lx_start(): ath10k's snoc.c
 * probes; its qmi.c makes the WLFW exchange; on "ready" the radio comes up).
 *
 * The supplies are not voted for (wifi/lx/src10/lx10_snoc.c): the 6T's boot leaves four of the five on (run 29).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);

#define WIFI10_FIRST_INTID      (414u + 32u)    /* dt: interrupts of wifi@18800000, GIC SPI 414..425: the GIC's number */
#define WIFI10_DDR_FIRST        0x80000000ull   /* what hakios_mpss_wifi_space() shows the radio, as it is */
#define WIFI10_DDR_END          0x400000000ull

extern void loader_log_putc(char c);  /* the loader's record of the run (what the run's log is made of) */

static void
say_line(const char *line)
{
	size_t length = strlen(line);

	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (length == 0 || line[length - 1] != '\n') {
		loader_log_putc('\n');
	}
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
}

int
hakios_wifi_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line);
	return 0;
}

int
hakios_wifi_tell(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line);
	return 0;
}
#define tell hakios_wifi_tell

uint32_t
hakios_wifi_option(const char *name)
{
	uint32_t value = 0;

	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

/* a file of /fw (fw-modem/ of the tree, staged by userland/build.sh MODEM=1) */
const uint8_t *
hakios_wifi_file(const char *name, uint64_t *size)
{
	return hakios_mpss_root_file(name, size);
}

bool
hakios_wifi_dsp_is_up(void)
{
	return hakios_mpss_qrtr() != NULL;
}

/* What ath10k asks for by Linux's name: ath10k/WCN3990/hw1.0/firmware-5.bin, board-2.bin (linux-firmware's) */
const uint8_t *
hakios_wifi_driver_file(const char *name, uint64_t *size)
{
	const uint8_t *file = hakios_mpss_root_file(name, size);

	printf("wifi: the driver asks for %s: %s (%llu bytes)\n", name, file != NULL ? "there" : "NOT THERE", file != NULL ? *size : 0);
	return file;
}

/* The radio's address of memory of ours: the same as its physical one (its SMMU context maps DDR as it is). */
bool
hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	if (physical < WIFI10_DDR_FIRST || physical + size > WIFI10_DDR_END) {
		tell("wifi: memory at %#llx (%#llx bytes) is outside what the radio is shown\n", physical, size);
		return false;
	}
	if (!hakios_mpss_wifi_space()) {
		tell("wifi: THE RADIO'S STREAM COULD NOT BE PUT THROUGH THE SMMU: it is given no memory\n");
		return false;
	}
	*address = physical;
	return true;
}

/* No MSI: the copy engines' interrupts are wired, levels (an address of 0 says so to hakios.lx.c). */
bool
hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi)
{
	*address = 0;
	*data = 0;
	*first_spi = WIFI10_FIRST_INTID;
	tell("wifi: the radio's interrupts are the GIC's %u..%u, wired, levels\n", WIFI10_FIRST_INTID, WIFI10_FIRST_INTID + 11);
	return true;
}

/* The radio's own address, if given (boot-arg wifi_mac, six bytes in hexadecimal with colons); otherwise its firmware's. */
bool
hakios_wifi_radio_address(uint8_t *address)
{
	char text[32] = "";
	uint8_t bytes[6];
	unsigned count = 0, digits = 0, value = 0;
	bool good = true;

	if (!PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) || text[0] == 0) {
		return false;
	}
	for (const char *c = text;; c++) {
		int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ?
		    *c - 'A' + 10 : -1;

		if (digit >= 0 && digits < 2) {
			value = value << 4 | (unsigned)digit;
			digits++;
		} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
			bytes[count++] = (uint8_t)value;
			digits = value = 0;
		} else {
			good = false;
		}
		if (*c == 0 || !good) {
			break;
		}
	}
	good = good && count == 6 && !(bytes[0] & 1) && (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5]) != 0;
	if (good) {
		memcpy(address, bytes, sizeof(bytes));
	}
	return good;
}

/* The network to join: /fw/network, the name on its first line and the passphrase on its second (W5; never logged). */
bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	const uint8_t *file = hakios_mpss_root_file("network", &size), *end, *line;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		return false;
	}
	end = file + size;
	line = file;
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return lengths[0] != 0;
}

/* ---- the start --------------------------------------------------------------------------------------------- */

static volatile bool wifi10_started;

static void
wifi10_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t wait_s = 300, watch_s = 60;
	int ret;

	PE_parse_boot_argn("wifi_wait", &wait_s, sizeof(wait_s));
	PE_parse_boot_argn("wifi_watch", &watch_s, sizeof(watch_s));
	for (uint32_t s = 0; s < wait_s * 10 && hakios_mpss_qrtr() == NULL; s++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	if (hakios_mpss_qrtr() == NULL) {
		tell("wifi: THE MODEM'S QRTR IS NOT UP after %u s (modem_step=10 modem_keep=1 brings it up): the radio is not started\n",
		    wait_s);
		thread_terminate(current_thread());
		return;
	}
	delay_for_interval(2000, NSEC_PER_MSEC);
	hakios_qmi_set_transport(hakios_mpss_qrtr());
	tell("wifi: the modem's QRTR is up: ath10k is started on the WCN3990 (the supplies as the boot leaves them)\n");
	wifi10_started = true;
	ret = hakios_lx_start();
	tell("wifi: the driver's start returns %d\n", ret);
	for (uint32_t s = 10; s <= watch_s; s += 10) {
		delay_for_interval(10000, NSEC_PER_MSEC);
		hakios_mpss_wifi_faults("watched");
	}
	thread_terminate(current_thread());
}

/* Called while the system starts. wifi=1: the radio, beside the system's start, once the modem's link is up. */
void
hakios_wifi_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi", &on, sizeof(on)) || on == 0) {
		return;
	}
	hakios_lx_net_early();
	if (kernel_thread_start(wifi10_thread, NULL, &thread) != KERN_SUCCESS) {
		tell("wifi: no thread: the radio is not brought up\n");
		return;
	}
	thread_deallocate(thread);
	tell("wifi: the 6T's radio (WCN3990, ath10k) waits for the modem's link, beside the system's start (wifi=1)\n");
}

/*
 * Where a run ends in order, before the modem is stopped (or when the phone is hot): the driver down. The radio's
 * stream stays in the SMMU: the radio's firmware runs on the modem and may still write what it was receiving after
 * the driver is down, and a write of a stream the SMMU does not know is fatal (run 65, the QCOM screen at a run's end
 * under a video's traffic); hakios_mpss_stop() puts it back once the modem is off.
 */
void
hakios_wifi_stop(void)
{
	if (wifi10_started) {
		hakios_lx_stop();
		wifi10_started = false;
	}
}

/* On a panic's way back to the loader: nothing that sleeps or takes a lock (the modem's own quiesce follows). */
void
hakios_wifi_quiesce(void)
{
}

/* For ath10k's qmi.c through the layer (wifi/lx/src10: qcom_scm_assign_mem()): the MSA's regions, by the hypervisor. */
int lx_os_assign_memory(unsigned long long address, unsigned long size, unsigned long long from, const int *vmid, const int *perm,
    unsigned count);

int
lx_os_assign_memory(unsigned long long address, unsigned long size, unsigned long long from, const int *vmid, const int *perm,
    unsigned count)
{
	return hakios_mpss_assign(address, size, from, vmid, perm, count) == 0 ? 0 : EINVAL;
}
