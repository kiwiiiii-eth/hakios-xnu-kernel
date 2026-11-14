/*
 * quietboot: the work Darwin does behind the screen, put in the background, so that what is on the screen has the processors.
 *
 * The system here is made afresh at every boot, as on a phone just restored: every widget's extension is run to describe
 * itself and draw its timeline, and the daemons behind them fetch and index, all at once. On the OnePlus 6T that took all
 * seven cores, 400 to 700 %, for the first 90 seconds after SpringBoard (r139, cputop: CalendarWidgetExtension 90 %,
 * WeatherWidget 85 %, StocksWidget, NewsToday2, WorldClockWidget, chronod, nsurlsessiond, amsengagementd, geod, ...), and
 * scrolling in those 90 seconds went by fits and starts. Here every process whose name says it is one of those is given
 * Darwin's background policy (setpriority PRIO_DARWIN_BG: the lowest scheduling band, its I/O throttled): it still runs,
 * and does its work, on what the screen leaves over. Looked for every two seconds for as long as the run lasts (a widget's
 * extension comes and goes), and given again each time (runningboardd may give it a role of its own meanwhile).
 *
 *   usage: quietboot [SECONDS]       every 2 s, SECONDS long (default: for good); a line for each process put back,
 *                                    and every 30 s how many are
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* libproc's, which the iPhoneOS SDK does not declare (as in cputop.c) */
#define PROC_ALL_PIDS           1
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

#ifndef PRIO_DARWIN_PROCESS
#define PRIO_DARWIN_PROCESS     4
#endif
#ifndef PRIO_DARWIN_BG
#define PRIO_DARWIN_BG          0x1000
#endif

/* names, or parts of names, of what works behind the screen (never SpringBoard, backboardd, an application in front) */
static const char *const behind[] = {
	"Widget",               /* every widget's extension: WeatherWidget, CalendarWidgetExtension, GCWidgets, ... */
	"NewsToday", "NewsTag",
	"chronod",              /* the widgets' timelines */
	"amsengagementd", "nsurlsessiond", "geod", "searchd", "suggestd", "assetsd", "intelligenceplatformd",
	"remindd", "itunescloudd", "appstored", "itunesstored", "promotedcontentd", "maild", "activityawardsd",
	"financed", "triald", "mapspushd", "gamed", "familycircled", "healthd", "photoanalysisd", "mediaanalysisd",
	"spotlightknowledged", "duetexpertd", "contextstored", "destinationd", "navd",
};

static int
is_behind(const char *name)
{
	for (size_t i = 0; i < sizeof(behind) / sizeof(behind[0]); i++) {
		if (strstr(name, behind[i]) != NULL) {
			return 1;
		}
	}
	return 0;
}

#define MAX_PIDS        4096

int
main(int argc, char **argv)
{
	static int pids[MAX_PIDS], told[MAX_PIDS];
	int told_count = 0, seconds = argc > 1 ? atoi(argv[1]) : 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("quietboot: what works behind the screen to the background, every 2 s%s\n", seconds > 0 ? "" : ", for good");
	for (int elapsed = 0; seconds <= 0 || elapsed < seconds; elapsed += 2) {
		int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
		int count = bytes > 0 ? bytes / (int)sizeof(int) : 0, now = 0;

		for (int i = 0; i < count; i++) {
			char name[64] = "";
			int pid = pids[i], known = 0;

			if (pid <= 1 || proc_name(pid, name, sizeof(name)) <= 0 || !is_behind(name)) {
				continue;
			}
			if (setpriority(PRIO_DARWIN_PROCESS, pid, PRIO_DARWIN_BG) != 0) {
				continue;
			}
			now++;
			for (int k = 0; k < told_count; k++) {
				known |= told[k] == pid;
			}
			if (!known && told_count < MAX_PIDS) {
				told[told_count++] = pid;
				printf("quietboot: %s[%d] to the background\n", name, pid);
			}
		}
		if (elapsed % 30 == 0) {
			printf("quietboot: %d s: %d processes in the background now\n", elapsed, now);
		}
		sleep(2);
	}
	return 0;
}
