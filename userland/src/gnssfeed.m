/*
 * gnssfeed: the modem's positions (the kernel's QMI LOC client, gnss=1: kern.hakios.gnss_*) handed to locationd, so that
 * Settings' Location Services, Maps and every application that asks CoreLocation get them.
 *
 * locationd here has none of an iPhone's GPS: the position goes in the way Xcode's "simulate location" puts one in, through
 * CoreLocation's CLSimulationManager (which wants the entitlement com.apple.locationd.simulation: src/gnssfeed.entitlements,
 * signed in by build.sh with ldid). Once a second the kernel's numbers are read; a new position (the count of reports went
 * up) replaces the simulated one. Only fixes (a report's session status 0) unless the kernel's arguments say
 * gnss_feed_any=1: then the modem's own position from before a fix too (r142: a point it remembered, 90 km off), to try
 * the way through indoors.
 *
 * Without gnss=1 among the kernel's arguments it ends at once; with it, it lets the system come up for a minute first, turns
 * Location Services on (the system is new at every boot, and locationd said "location disabled system-wide" to everybody,
 * r145), and hands a position when it moves, or the same one again every 30 s (locationd may come after it, and is started
 * on demand): the modem reports one a second.
 *
 *   usage: gnssfeed            (from a launchd job: userland/rc.jobs.*)
 */
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#include <objc/message.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int
kernel_int(const char *name, int otherwise)
{
	int value = 0;
	size_t size = sizeof(value);

	return sysctlbyname(name, &value, &size, NULL, 0) == 0 ? value : otherwise;
}

static BOOL
boot_arg(const char *word)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, word) != NULL;
}

int
main(void)
{
	@autoreleasepool {
		Class simulation = NSClassFromString(@"CLSimulationManager");
		id manager = simulation != Nil ? [[simulation alloc] init] : nil;
		BOOL any = boot_arg("gnss_feed_any=1");
		int seen = 0, fed = 0, said_waiting = 0, since = 0, last_latitude = 0, last_longitude = 0;

		setvbuf(stdout, NULL, _IOLBF, 0);
		if (!boot_arg("gnss=1")) {
			return 0;
		}
		sleep(60);
		if (manager == nil) {
			printf("gnssfeed: CoreLocation has no CLSimulationManager here: nothing to hand positions to\n");
			return 1;
		}
		printf("gnssfeed: handing the modem's %s to locationd\n", any ? "positions, fixes or not (gnss_feed_any=1)," : "fixes");
		{
			Class locations = NSClassFromString(@"CLLocationManager");
			SEL enable = NSSelectorFromString(@"setLocationServicesEnabled:"), enabled = NSSelectorFromString(@"locationServicesEnabled");

			if (locations != Nil && [locations respondsToSelector:enable]) {
				((void (*)(id, SEL, BOOL))objc_msgSend)(locations, enable, YES);
				printf("gnssfeed: Location Services turned on: now %s\n",
				    ((BOOL (*)(id, SEL))objc_msgSend)(locations, enabled) ? "on" : "STILL OFF");
			}
		}
		for (;;) {
			int reports = kernel_int("kern.hakios.gnss_reports", -1), status = kernel_int("kern.hakios.gnss_status", -1);

			if (reports < 0) {
				printf("gnssfeed: the kernel says nothing of a position (no gnss=1?): stopping\n");
				return 0;
			}
			int latitude_e6 = kernel_int("kern.hakios.gnss_latitude_e6", 0), longitude_e6 = kernel_int("kern.hakios.gnss_longitude_e6", 0);

			since++;
			if (reports > seen && (status == 0 || any) &&
			    (fed == 0 || since >= 30 || latitude_e6 != last_latitude || longitude_e6 != last_longitude)) {
				double latitude = latitude_e6 / 1e6, longitude = longitude_e6 / 1e6;
				int accuracy = kernel_int("kern.hakios.gnss_accuracy_m", 0);
				CLLocation *location = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(latitude, longitude)
				    altitude:0 horizontalAccuracy:(accuracy > 0 ? accuracy : (status == 0 ? 20 : 5000)) verticalAccuracy:-1
				    timestamp:[NSDate date]];

				seen = reports;
				since = 0;
				last_latitude = latitude_e6;
				last_longitude = longitude_e6;
				((void (*)(id, SEL))objc_msgSend)(manager, @selector(stopLocationSimulation));
				((void (*)(id, SEL))objc_msgSend)(manager, @selector(clearSimulatedLocations));
				((void (*)(id, SEL, id))objc_msgSend)(manager, @selector(appendSimulatedLocation:), location);
				((void (*)(id, SEL))objc_msgSend)(manager, @selector(flush));
				((void (*)(id, SEL))objc_msgSend)(manager, @selector(startLocationSimulation));
				if (fed++ < 5 || fed % 60 == 0) {
					printf("gnssfeed: %d: %.6f, %.6f within %d m (session status %d) to locationd\n", fed, latitude, longitude,
					    accuracy, status);
				}
			} else if (fed == 0 && said_waiting++ % 60 == 0) {
				printf("gnssfeed: waiting for a %s (%d reports so far, status %d, %d satellites in view)\n",
				    any ? "position" : "fix", reports, status, kernel_int("kern.hakios.gnss_in_view", 0));
			}
			sleep(1);
		}
	}
}
