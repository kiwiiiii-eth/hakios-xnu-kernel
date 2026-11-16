/*
 * devname: the phone's name set the way Settings > General > About > Name sets it (GeneralSettingsUI's
 * -[PSGAboutDataSource setDeviceName:specifier:] calls Preferences.framework's SetDeviceName(), which hands it to
 * lockdownd), and read back (DeviceName(), MobileGestalt's UserAssignedDeviceName): 16 November, a name typed in
 * Settings did not stay -- lockdownd was not among the jobs (LOCKDOWN=1 in build.sh).
 *
 *   usage: devname [NAME]        without NAME: only what it is now
 *          devname --test NAME   set NAME, read it back, then put back the name there was
 * Signed with Preferences' entitlements (src/devname.entitlements), which profiled asks of its callers.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);

static void
say_name(const char *when)
{
	CFTypeRef name = MGCopyAnswer(CFSTR("UserAssignedDeviceName"));

	printf("devname: %s: MobileGestalt says \"%s\"\n", when,
	    name && CFGetTypeID(name) == CFStringGetTypeID() ? [(__bridge NSString *)name UTF8String] : "(none)");
	if (name) {
		CFRelease(name);
	}
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		BOOL test = argc > 2 && strcmp(argv[1], "--test") == 0;
		NSString *wanted = argc > 1 ? @(argv[argc - 1]) : nil;
		CFTypeRef before_value = MGCopyAnswer(CFSTR("UserAssignedDeviceName"));
		NSString *before = before_value && CFGetTypeID(before_value) == CFStringGetTypeID() ? (__bridge_transfer NSString *)before_value : nil;

		say_name("before");
		if (wanted == nil) {
			return 0;
		}
		void *preferences = dlopen("/System/Library/PrivateFrameworks/Preferences.framework/Preferences", RTLD_NOW);
		void (*set_device_name)(NSString *) = preferences ? dlsym(preferences, "SetDeviceName") : NULL;
		NSString *(*device_name)(void) = preferences ? dlsym(preferences, "DeviceName") : NULL;

		if (set_device_name == NULL || device_name == NULL) {
			printf("devname: no SetDeviceName/DeviceName in Preferences.framework\n");
			return 1;
		}
		printf("devname: DeviceName() says \"%s\"\n", device_name().UTF8String);
		printf("devname: SetDeviceName(\"%s\")\n", wanted.UTF8String);
		set_device_name(wanted);
		sleep(3);
		printf("devname: DeviceName() says \"%s\"\n", device_name().UTF8String);
		say_name("after");
		if (test && before != nil) {
			set_device_name(before);
			sleep(3);
			say_name("put back");
		}
		return 0;
	}
}
