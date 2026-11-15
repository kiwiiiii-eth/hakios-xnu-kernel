/*
 * rtcclock: the date for a phone that boots with no host to give it (Beta 0.4: from its own boot partition).
 *
 * A run from the Mac gives the kernel the Mac's time (epoch= on its command line). A phone that boots by itself has
 * only what its boot image was built with, which is the time it was built. The PMIC's real-time clock goes on while the
 * phone is off, but it is a count of seconds, not the date (kern.hakios.rtc_now, drivers/power/hakios.fg8998.c). So,
 * as Android's time service does, the offset between the two is kept in a file -- /private/var/db/hakios-rtc-offset,
 * on the data volume that var_store keeps across boots -- and:
 *
 *   at start   the clock is set to the RTC's count plus that offset, if the file is there and they differ by more than
 *              two seconds (the first boot after an install has no file, and keeps the image's time);
 *   then       every five minutes (the first time after one), the offset as it is now -- the clock less the count -- is
 *              written to the file if it has moved by more than two seconds: when iOS has set the clock from the
 *              network, or by hand in Settings, the next boot starts from that.
 *
 * The count has been seen to start again from zero: twice on 2026-11-15, at 12:31:30 and at 15:13:50, both times the
 * phone was got out of a stuck loader with Power and Volume Up held (a restart from iOS, ABL or the loader does not, nor
 * did the same buttons held in iOS at 15:52; what does is not known). The count plus the old offset is then a time long
 * past -- the boot after the second was set back from the image's 14:47 to 12:38 until iOS had the network's time. So the file keeps the count it was written at as well
 * ("offset count"; a file of the offset alone is taken without it), and the clock is left as it is when
 *
 *   - the count is lower than that: it was started again;
 *   - the count and the offset would set the clock back: the image was built before the boot, so the time it starts
 *     with is never later than the true one.
 *
 * It never sets the clock but once, at start; iOS's own time service corrects it afterwards as on any iPhone.
 *
 *   usage: rtcclock            (from a launchd job's rc.jobs line, in the background)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#define OFFSET_FILE "/private/var/db/hakios-rtc-offset"
#define SLACK       2

static int
rtc_now(long long *seconds)
{
	long long value = -1;
	size_t size = sizeof(value);

	if (sysctlbyname("kern.hakios.rtc_now", &value, &size, NULL, 0) != 0 || value < 0) {
		return 0;
	}
	*seconds = value;
	return 1;
}

/* the offset kept, and the count it was written at (-1: a file from before the count was kept) */
static int
offset_read(long long *offset, long long *count)
{
	FILE *f = fopen(OFFSET_FILE, "r");
	int got;

	if (f == NULL) {
		return 0;
	}
	*count = -1;
	got = fscanf(f, "%lld %lld", offset, count) >= 1;
	fclose(f);
	return got;
}

static int
offset_write(long long offset, long long count)
{
	char temporary[] = OFFSET_FILE ".new";
	FILE *f = fopen(temporary, "w");

	if (f == NULL) {
		return 0;
	}
	fprintf(f, "%lld %lld\n", offset, count);
	if (fclose(f) != 0 || rename(temporary, OFFSET_FILE) != 0) {
		unlink(temporary);
		return 0;
	}
	return 1;
}

int
main(void)
{
	long long rtc = 0, offset = 0, count = -1;
	int have = 0, waited = 0;
	struct timeval now;

	setvbuf(stdout, NULL, _IOLBF, 0);
	/* the battery's thread reads the clock every ten seconds from its start */
	for (int i = 0; i < 60 && !rtc_now(&rtc); i++) {
		sleep(1);
	}
	if (!rtc_now(&rtc)) {
		printf("rtcclock: the kernel has no reading of the real-time clock (kern.hakios.rtc_now): the clock is left as it is\n");
		return 0;
	}
	gettimeofday(&now, NULL);
	have = offset_read(&offset, &count);
	if (have && count >= 0 && rtc + SLACK < count) {
		printf("rtcclock: the real-time clock counts %lld, less than the %lld the offset was kept at: it was started "
		    "again: the clock is left as it is, and the offset kept again\n", rtc, count);
		have = 0;
	} else if (have && rtc + offset + SLACK < (long long)now.tv_sec) {
		printf("rtcclock: the real-time clock and the kept offset would set the clock back %lld s, before the time "
		    "this image starts with: the count was started again: the clock is left as it is, and the offset kept again\n",
		    (long long)now.tv_sec - (rtc + offset));
		have = 0;
	} else if (have) {
		long long target = rtc + offset;

		if (llabs(target - (long long)now.tv_sec) > SLACK) {
			struct timeval set = { .tv_sec = (time_t)target, .tv_usec = 0 };

			if (settimeofday(&set, NULL) == 0) {
				printf("rtcclock: the clock set from the real-time clock and the kept offset: %+lld s\n",
				    target - (long long)now.tv_sec);
			} else {
				printf("rtcclock: setting the clock: %s\n", strerror(errno));
			}
		} else {
			printf("rtcclock: the clock agrees with the real-time clock and the kept offset\n");
		}
	} else {
		printf("rtcclock: no offset kept yet (%s): the clock is left as it is, and the offset kept from now on\n",
		    OFFSET_FILE);
	}
	for (;;) {
		sleep(waited++ == 0 ? 60 : 300);
		if (!rtc_now(&rtc)) {
			continue;
		}
		gettimeofday(&now, NULL);
		long long current = (long long)now.tv_sec - rtc;

		if (!have || llabs(current - offset) > SLACK) {
			if (offset_write(current, rtc)) {
				if (have) {
					printf("rtcclock: the offset kept moves by %+lld s\n", current - offset);
				}
				offset = current;
				have = 1;
			}
		}
	}
}
