/*
 * celltz: the time zone from the cellular network, as an iPhone's Settings > General > Date & Time > Set Automatically
 * has it. An iPhone takes it from CommCenter (the network's NITZ) and from Location Services; on the 6T CommCenter does
 * not run and an outdoor fix has not been seen, so iOS's own automatic time zone found nothing. The kernel's modem
 * driver reads what the network tells instead -- the country (MCC) and the offset (QMI NAS serving system, NITZ:
 * kern.hakios.cellular_mcc, cellular_tz_quarters, cellular_dst) -- and this sets the system's time zone from them:
 *
 *   - only while Set Automatically is on (/var/db/timed/com.apple.timed.plist, AutomaticTimeZoneEnabled; on when the
 *     key is not there, as on a new iPhone): a zone chosen by hand is never changed;
 *   - the zone: the country's own when it has one (466, Taiwan: Asia/Taipei), else the one of its zones with the
 *     offset the network told, else "Etc/GMT-N" for a whole-hour offset; nothing when there is no country or offset;
 *   - set through tzlinkd (libutil's tzlink(), entitlement com.apple.tzlink.allow: src/celltz.entitlements), the way
 *     Settings does it, and only when the zone in force is another.
 *
 * Looked at every 30 s for as long as it runs (a new country is a new zone).
 *
 *   usage: celltz          (from a launchd job's rc.jobs line, in the background)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <limits.h>
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define TIMED_PREFERENCES "/var/db/timed/com.apple.timed.plist"
#define LOCALTIME         "/var/db/timezone/localtime"

/* countries with one time zone (the ones a 6T's owner is likely to take it to; others fall to the offset) */
static const struct { int mcc; const char *zone; } single[] = {
	{ 466, "Asia/Taipei" }, { 440, "Asia/Tokyo" }, { 441, "Asia/Tokyo" }, { 450, "Asia/Seoul" }, { 460, "Asia/Shanghai" },
	{ 461, "Asia/Shanghai" }, { 454, "Asia/Hong_Kong" }, { 455, "Asia/Macau" }, { 525, "Asia/Singapore" },
	{ 502, "Asia/Kuala_Lumpur" }, { 520, "Asia/Bangkok" }, { 515, "Asia/Manila" }, { 452, "Asia/Ho_Chi_Minh" },
	{ 456, "Asia/Phnom_Penh" }, { 457, "Asia/Vientiane" }, { 414, "Asia/Yangon" }, { 404, "Asia/Kolkata" },
	{ 405, "Asia/Kolkata" }, { 470, "Asia/Dhaka" }, { 413, "Asia/Colombo" }, { 424, "Asia/Dubai" }, { 425, "Asia/Jerusalem" },
	{ 420, "Asia/Riyadh" }, { 427, "Asia/Qatar" }, { 286, "Europe/Istanbul" }, { 234, "Europe/London" },
	{ 235, "Europe/London" }, { 272, "Europe/Dublin" }, { 208, "Europe/Paris" }, { 262, "Europe/Berlin" },
	{ 222, "Europe/Rome" }, { 214, "Europe/Madrid" }, { 268, "Europe/Lisbon" }, { 204, "Europe/Amsterdam" },
	{ 206, "Europe/Brussels" }, { 228, "Europe/Zurich" }, { 232, "Europe/Vienna" }, { 230, "Europe/Prague" },
	{ 260, "Europe/Warsaw" }, { 238, "Europe/Copenhagen" }, { 240, "Europe/Stockholm" }, { 242, "Europe/Oslo" },
	{ 244, "Europe/Helsinki" }, { 202, "Europe/Athens" }, { 216, "Europe/Budapest" }, { 226, "Europe/Bucharest" },
	{ 530, "Pacific/Auckland" },
};

/* countries with several, by the offset the network tells (quarters of an hour) */
static const struct { int mcc, quarters; const char *zone; } several[] = {
	{ 310, -20, "America/New_York" }, { 310, -16, "America/New_York" }, { 310, -24, "America/Chicago" },
	{ 310, -28, "America/Denver" }, { 310, -32, "America/Los_Angeles" }, { 310, -36, "America/Anchorage" },
	{ 310, -40, "Pacific/Honolulu" }, { 302, -20, "America/Toronto" }, { 302, -16, "America/Toronto" },
	{ 302, -24, "America/Winnipeg" }, { 302, -28, "America/Edmonton" }, { 302, -32, "America/Vancouver" },
	{ 505, 40, "Australia/Sydney" }, { 505, 44, "Australia/Sydney" }, { 505, 38, "Australia/Adelaide" },
	{ 505, 42, "Australia/Adelaide" }, { 505, 36, "Australia/Darwin" }, { 505, 32, "Australia/Perth" },
	{ 510, 28, "Asia/Jakarta" }, { 510, 32, "Asia/Makassar" }, { 510, 36, "Asia/Jayapura" },
	{ 250, 12, "Europe/Moscow" }, { 724, -12, "America/Sao_Paulo" }, { 334, -24, "America/Mexico_City" },
};

static int
kernel_int(const char *name, int otherwise)
{
	int value = otherwise;
	size_t size = sizeof(value);

	return sysctlbyname(name, &value, &size, NULL, 0) == 0 ? value : otherwise;
}

/* Set Automatically, as timed keeps it: 1 on (or not said), 0 off */
static int
automatic(void)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)TIMED_PREFERENCES,
	    (CFIndex)strlen(TIMED_PREFERENCES), false);
	CFReadStreamRef stream = url != NULL ? CFReadStreamCreateWithFile(NULL, url) : NULL;
	CFPropertyListRef plist = NULL;
	int on = 1;

	if (stream != NULL && CFReadStreamOpen(stream)) {
		plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
		CFReadStreamClose(stream);
	}
	if (plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
		CFTypeRef value = CFDictionaryGetValue(plist, CFSTR("AutomaticTimeZoneEnabled"));

		if (value != NULL && CFGetTypeID(value) == CFBooleanGetTypeID()) {
			on = CFBooleanGetValue(value);
		}
	}
	if (plist != NULL) CFRelease(plist);
	if (stream != NULL) CFRelease(stream);
	if (url != NULL) CFRelease(url);
	return on;
}

static const char *
zone_for(int mcc, int quarters, char *buffer, size_t size)
{
	for (unsigned i = 0; i < sizeof(single) / sizeof(single[0]); i++) {
		if (single[i].mcc == mcc) {
			return single[i].zone;
		}
	}
	for (unsigned i = 0; i < sizeof(several) / sizeof(several[0]); i++) {
		/* (the US is 310 to 316) */
		int country = mcc >= 310 && mcc <= 316 ? 310 : mcc;

		if (several[i].mcc == country && several[i].quarters == quarters) {
			return several[i].zone;
		}
	}
	if (quarters != -128 && quarters % 4 == 0 && quarters != 0) {
		/* (POSIX's sign: Etc/GMT-8 is eight hours ahead of UTC) */
		snprintf(buffer, size, "Etc/GMT%+d", -quarters / 4);
		return buffer;
	}
	return quarters == 0 ? "Etc/UTC" : NULL;
}

int
main(void)
{
	int (*tzlink)(const char *) = NULL;
	void *libutil = dlopen("/usr/lib/libutil.dylib", RTLD_NOW);
	char said[128] = "";

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (libutil != NULL) {
		tzlink = (int (*)(const char *))dlsym(libutil, "tzlink");
	}
	if (tzlink == NULL) {
		printf("celltz: no tzlink() in libutil: the time zone cannot be set\n");
		return 1;
	}
	for (;;) {
		int mcc = kernel_int("kern.hakios.cellular_mcc", 0), quarters = kernel_int("kern.hakios.cellular_tz_quarters", -128);
		char buffer[32], have[PATH_MAX] = "", line[128];
		const char *zone = mcc != 0 || quarters != -128 ? zone_for(mcc, quarters, buffer, sizeof(buffer)) : NULL;
		ssize_t n = readlink(LOCALTIME, have, sizeof(have) - 1);

		if (n > 0) {
			have[n] = 0;
		}
		if (zone == NULL) {
			snprintf(line, sizeof(line), "no time zone from the network yet (country %d, offset %d/4 h)", mcc, quarters);
		} else if (!automatic()) {
			snprintf(line, sizeof(line), "Set Automatically is off: the zone chosen by hand stays (the network says %s)", zone);
		} else if (n > 0 && strlen(have) >= strlen(zone) && strcmp(have + strlen(have) - strlen(zone), zone) == 0 &&
		    (strlen(have) == strlen(zone) || have[strlen(have) - strlen(zone) - 1] == '/')) {
			snprintf(line, sizeof(line), "the time zone is %s, as the network says (country %d)", zone, mcc);
		} else {
			int error = tzlink(zone);

			if (error == 0) {
				notify_post("SignificantTimeChangeNotification");
				snprintf(line, sizeof(line), "the time zone set to %s from the network (country %d, offset %d/4 h)", zone,
				    mcc, quarters);
			} else {
				snprintf(line, sizeof(line), "tzlink(%s): %s", zone, strerror(error));
			}
		}
		if (strcmp(line, said) != 0) {
			printf("celltz: %s\n", line);
			snprintf(said, sizeof(said), "%s", line);
		}
		sleep(30);
	}
}
