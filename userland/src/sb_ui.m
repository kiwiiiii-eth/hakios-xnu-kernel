/*
 * libhakios.sbui.dylib: two things of SpringBoard's own UI for this phone, opened by libhakios.sb.dylib (src/inject_sb.c).
 *
 *   The power menu   power and volume down held together for 1.2 s bring up iOS's own "slide to power off" (SpringBoard's
 *                    presentPowerDownTransientOverlay), as on an iPhone. The buttons' presses still go to SpringBoard's own
 *                    handler first (SBSOSClawGestureObserver, Emergency SOS's): nothing of it is cancelled or called by us,
 *                    and a short press of both does nothing.
 *   The volume HUD   drawn beside the 6T's volume buttons, which sit lower than an iPhone 14 Pro's: boot-arg
 *                    hud_button_offset=POINTS moves it down (up if negative; 0: where iOS puts it), 210 by default
 *                    (48 was still about a long and a short button too high on the phone, some 32 mm at 5.76 points a mm; 230 a little low).
 *
 *   The hotspot      as an iPhone shows its Personal Hotspot (25 November): the status bar's green bubble while stations
 *                    are joined (SpringBoard's own, told through SBTelephonyManager), and Control Center's Personal Hotspot
 *                    button, which turns ours on and off. src/hotspot.c posts com.hakios.hotspot.stations on every change;
 *                    the numbers are the kernel's (kern.hakios.hotspot, kern.hakios.hotspot_stations), the switch Settings'
 *                    (com.hakios.hotspot.conf's "on=", src/settings_ui.m).
 *
 * From the diagnostic adapters of 2026-11-23 (power_controls, ui_adapter), phone-tested in RAM runs.
 */
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define BUTTON_VOLUME_DOWN      103     /* SpringBoard's kinds of button, as noteButtonPress:isDown: gets them */
#define BUTTON_LOCK             104
#define CHORD_HOLD_MS           1200
#define HUD_OFFSET_DEFAULT      210

static int console = -1, hud_offset;
static BOOL power_down, volume_down;
static unsigned chord;
static void (*original_press)(id, SEL, long long, BOOL);
static double (*original_hud_center)(id, SEL);

static void
say(NSString *text)
{
	if (console < 0) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	const char *line = [@"sbui: " stringByAppendingFormat:@"%@\n", text].UTF8String;
	if (line != NULL) {
		(void)write(console >= 0 ? console : 2, line, strlen(line));
	}
}

static int
boot_arg(const char *key, int otherwise)
{
	char args[2048] = { 0 };
	size_t size = sizeof(args) - 1, n = strlen(key);

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return otherwise;
	}
	for (const char *p = args; (p = strstr(p, key)) != NULL; p += n) {
		if ((p == args || p[-1] == ' ') && p[n] == '=') {
			return atoi(p + n + 1);
		}
	}
	return otherwise;
}

/* CLS's SEL takes ARGUMENTS (self and _cmd counted) and returns RESULT */
static BOOL
shaped(Class cls, SEL sel, unsigned arguments, const char *result)
{
	Method method = class_getInstanceMethod(cls, sel);

	if (method == NULL || method_getNumberOfArguments(method) != arguments) {
		return NO;
	}
	char *type = method_copyReturnType(method);
	BOOL same = type != NULL && strcmp(type, result) == 0;
	free(type);
	return same;
}

static void
swap(Class cls, SEL sel, IMP implementation, IMP *previous)
{
	Method method = class_getInstanceMethod(cls, sel);

	*previous = method_getImplementation(method);
	if (!class_addMethod(cls, sel, implementation, method_getTypeEncoding(method))) {
		method_setImplementation(class_getInstanceMethod(cls, sel), implementation);
	}
}


/* ---- the power menu ---- */

static id
workspace(void)
{
	Class cls = NSClassFromString(@"SBMainWorkspace");
	SEL shared = NSSelectorFromString(@"sharedInstanceIfExists");

	return [cls respondsToSelector:shared] ? ((id (*)(id, SEL))objc_msgSend)(cls, shared) : nil;
}

static BOOL
menu_up(id ws)
{
	Ivar ivar = ws != nil ? class_getInstanceVariable(object_getClass(ws), "_powerDownViewController") : NULL;
	const char *type = ivar != NULL ? ivar_getTypeEncoding(ivar) : NULL;

	return type != NULL && type[0] == '@' && object_getIvar(ws, ivar) != nil;
}

static void
present_menu(void)
{
	id ws = workspace();
	SEL present = NSSelectorFromString(@"presentPowerDownTransientOverlay");

	if (ws == nil || !shaped(object_getClass(ws), present, 2, "v")) {
		say(@"power menu: SpringBoard has no presentPowerDownTransientOverlay");
		return;
	}
	if (menu_up(ws)) {
		return;
	}
	((void (*)(id, SEL))objc_msgSend)(ws, present);
	say(@"power menu: up");
}

static void
button_pressed(id self, SEL command, long long kind, BOOL down)
{
	original_press(self, command, kind, down);
	dispatch_async(dispatch_get_main_queue(), ^{
		BOOL changed = NO;

		if (kind == BUTTON_LOCK && power_down != down) {
			power_down = down;
			changed = YES;
		}
		if (kind == BUTTON_VOLUME_DOWN && volume_down != down) {
			volume_down = down;
			changed = YES;
		}
		if (!changed) {
			return;
		}
		unsigned which = ++chord;
		if (power_down && volume_down) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, CHORD_HOLD_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
				if (which == chord && power_down && volume_down) {
					present_menu();
				}
			});
		}
	});
}

static void
power_menu(void)
{
	Class cls = NSClassFromString(@"SBSOSClawGestureObserver");
	SEL press = NSSelectorFromString(@"noteButtonPress:isDown:");
	Method method = cls != Nil ? class_getInstanceMethod(cls, press) : NULL;
	char *kind, *down;
	BOOL fits;

	if (method == NULL || !shaped(cls, press, 4, "v")) {
		say(@"power menu: no SBSOSClawGestureObserver noteButtonPress:isDown:, so none");
		return;
	}
	kind = method_copyArgumentType(method, 2);
	down = method_copyArgumentType(method, 3);
	fits = kind != NULL && down != NULL && strcmp(kind, "q") == 0 && strcmp(down, "B") == 0;
	free(kind);
	free(down);
	if (!fits) {
		say(@"power menu: noteButtonPress:isDown: takes something else, so none");
		return;
	}
	swap(cls, press, (IMP)button_pressed, (IMP *)&original_press);
	int token;
	notify_register_dispatch("com.hakios.power.menu", &token, dispatch_get_main_queue(), ^(__unused int t) {
		present_menu();
	});
	say(@"power menu: power and volume down, held");
}


/* ---- the volume HUD ---- */

static double
hud_center(id self, SEL command)
{
	double before = original_hud_center(self, command), after = before + hud_offset;

	return isfinite(before) && before >= 0 && after >= 0 && after <= 700 ? after : before;
}

static void
volume_hud(void)
{
	Class cls = NSClassFromString(@"SBElasticHUDSettings");
	SEL center = NSSelectorFromString(@"volumeButtonsCenterY");

	hud_offset = boot_arg("hud_button_offset", HUD_OFFSET_DEFAULT);
	if (hud_offset < -400 || hud_offset > 400) {
		hud_offset = HUD_OFFSET_DEFAULT;
	}
	if (hud_offset == 0) {
		return;
	}
	if (!shaped(cls, center, 2, "d")) {
		say(@"volume HUD: no SBElasticHUDSettings volumeButtonsCenterY, so where iOS puts it");
		return;
	}
	swap(cls, center, (IMP)hud_center, (IMP *)&original_hud_center);
	say([NSString stringWithFormat:@"volume HUD: %+d points", hud_offset]);
}

/* ---- the hotspot: the status bar and Control Center ---- */

#define HOTSPOT_CONF            @"/private/var/mobile/Library/Preferences/com.hakios.hotspot.conf"
#define HOTSPOT_CHANGED         "com.hakios.hotspot.changed"
#define HOTSPOT_STATIONS        "com.hakios.hotspot.stations"

static NSHashTable *hotspot_buttons;            /* Control Center's Personal Hotspot buttons, as they come */
static BOOL (*original_hotspot_supported)(id, SEL);
static BOOL control_center_hooked;

static int
kernel_number(const char *name, int otherwise)
{
	int value = 0;
	size_t size = sizeof(value);

	return sysctlbyname(name, &value, &size, NULL, 0) == 0 ? value : otherwise;
}

/* Settings' switch, Allow Others to Join (CONF's "on="); the other lines as they are */
static NSMutableArray *
hotspot_conf_lines(void)
{
	NSString *text = [NSString stringWithContentsOfFile:HOTSPOT_CONF encoding:NSUTF8StringEncoding error:nil];

	return text != nil ? [[text componentsSeparatedByString:@"\n"] mutableCopy] : nil;
}

static BOOL
hotspot_wanted(void)
{
	for (NSString *line in hotspot_conf_lines()) {
		if ([line hasPrefix:@"on="]) {
			return [[line substringFromIndex:3] intValue] != 0;
		}
	}
	return NO;
}

static BOOL
hotspot_set_wanted(BOOL on)
{
	NSMutableArray *lines = hotspot_conf_lines();
	BOOL found = NO;

	if (lines == nil) {
		return NO;              /* (src/hotspot.c makes CONF at the start; without it, nothing to switch) */
	}
	for (NSUInteger i = 0; i < lines.count; i++) {
		if ([lines[i] hasPrefix:@"on="]) {
			lines[i] = on ? @"on=1" : @"on=0";
			found = YES;
		}
	}
	if (!found) {
		[lines insertObject:on ? @"on=1" : @"on=0" atIndex:0];
	}
	if (![[lines componentsJoinedByString:@"\n"] writeToFile:HOTSPOT_CONF atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
		return NO;
	}
	(void)chmod(HOTSPOT_CONF.fileSystemRepresentation, 0600);
	notify_post(HOTSPOT_CHANGED);
	say([NSString stringWithFormat:@"hotspot: Control Center turns it %@", on ? @"on" : @"off"]);
	return YES;
}

/* SpringBoard's own tethering state: its status bar draws the green bubble while devices are joined */
static void
hotspot_status_bar(BOOL on, int stations)
{
	static int said = -1;
	Class cls = objc_getClass("SBTelephonyManager");
	SEL shared = sel_registerName("sharedTelephonyManager"), set = sel_registerName("_setIsNetworkTethering:withNumberOfDevices:");
	id manager = cls != Nil && class_getClassMethod(cls, shared) != NULL ? ((id (*)(Class, SEL))objc_msgSend)(cls, shared) : nil;
	int now = on ? stations : 0;

	if (manager == nil || !shaped(object_getClass(manager), set, 4, "v")) {
		if (said != -2) {
			say(@"hotspot: SBTelephonyManager is not what it was: no green bubble");
			said = -2;
		}
		return;
	}
	((void (*)(id, SEL, BOOL, int))objc_msgSend)(manager, set, now > 0, now);
	if (now != said) {
		say([NSString stringWithFormat:@"hotspot: the status bar: %d device%s joined", now, now == 1 ? "" : "s"]);
		said = now;
	}
}

static void
hotspot_button_update(id button)
{
	BOOL on = hotspot_wanted() && kernel_number("kern.hakios.hotspot", 0) != 0;
	unsigned stations = (unsigned)kernel_number("kern.hakios.hotspot_stations", 0);

	if (button == nil) {
		return;
	}
	[hotspot_buttons addObject:button];
	((void (*)(id, SEL, BOOL))objc_msgSend)(button, sel_registerName("setAvailable:"), YES);
	((void (*)(id, SEL, BOOL))objc_msgSend)(button, sel_registerName("setDiscoverable:"), on || hotspot_wanted());
	((void (*)(id, SEL, unsigned))objc_msgSend)(button, sel_registerName("setConnections:"), on ? stations : 0);
	((void (*)(id, SEL, BOOL))objc_msgSend)(button, sel_registerName("setEnabled:"), hotspot_wanted());
	if ([button respondsToSelector:sel_registerName("setInoperative:")]) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(button, sel_registerName("setInoperative:"), NO);
	}
	if ([button respondsToSelector:sel_registerName("subtitleText")] && [button respondsToSelector:sel_registerName("setSubtitle:")]) {
		id text = ((id (*)(id, SEL))objc_msgSend)(button, sel_registerName("subtitleText"));
		if (text != nil) {
			((void (*)(id, SEL, id))objc_msgSend)(button, sel_registerName("setSubtitle:"), text);
		}
	}
}

static BOOL
hotspot_supported(id self, SEL command)
{
	return kernel_number("kern.hakios.hotspot", -1) >= 0 ? YES :
	    original_hotspot_supported != NULL && original_hotspot_supported(self, command);
}

static void
hotspot_update_state(id self, __unused SEL command)
{
	hotspot_button_update(self);
}

static void
hotspot_toggle(id self, __unused SEL command)
{
	(void)hotspot_set_wanted(!hotspot_wanted());
	hotspot_button_update(self);
}

/* the button's tap (iOS's checks the carrier's provisioning and Wi-Fi's own state first, which say no here) */
static void
hotspot_tapped(id self, SEL command, __unused id sender)
{
	hotspot_toggle(self, command);
}

/* Control Center's module comes in its own bundle, when it is first wanted: hooked then */
static void
hotspot_hook_control_center(void)
{
	Class cls = objc_getClass("CCUIConnectivityHotspotViewController");
	IMP unused;

	if (control_center_hooked || cls == Nil) {
		return;
	}
	control_center_hooked = YES;
	Method supported = class_getClassMethod(cls, sel_registerName("isSupported"));
	if (supported != NULL) {
		original_hotspot_supported = (BOOL (*)(id, SEL))method_getImplementation(supported);
		method_setImplementation(supported, (IMP)hotspot_supported);
	}
	if (shaped(cls, sel_registerName("_updateState"), 2, "v")) {
		swap(cls, sel_registerName("_updateState"), (IMP)hotspot_update_state, &unused);
	}
	if (shaped(cls, sel_registerName("_toggleState"), 2, "v")) {
		swap(cls, sel_registerName("_toggleState"), (IMP)hotspot_toggle, &unused);
	}
	if (shaped(cls, sel_registerName("buttonTapped:"), 3, "v")) {
		swap(cls, sel_registerName("buttonTapped:"), (IMP)hotspot_tapped, &unused);
	}
	say(@"hotspot: Control Center's Personal Hotspot button is ours");
}

static void
hotspot_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	if (!control_center_hooked) {
		dispatch_async(dispatch_get_main_queue(), ^{
			hotspot_hook_control_center();
		});
	}
}

static void
hotspot_refresh(void)
{
	BOOL on = kernel_number("kern.hakios.hotspot", 0) != 0;

	hotspot_status_bar(on, kernel_number("kern.hakios.hotspot_stations", 0));
	for (id button in hotspot_buttons.allObjects) {
		hotspot_button_update(button);
	}
}

static void
hotspot(void)
{
	int token;

	if (kernel_number("kern.hakios.hotspot", -1) < 0) {
		return;                 /* (no Wi-Fi in this kernel) */
	}
	hotspot_buttons = [NSHashTable weakObjectsHashTable];
	notify_register_dispatch(HOTSPOT_STATIONS, &token, dispatch_get_main_queue(), ^(__unused int t) {
		hotspot_refresh();
	});
	notify_register_dispatch(HOTSPOT_CHANGED, &token, dispatch_get_main_queue(), ^(__unused int t) {
		hotspot_refresh();
	});
	_dyld_register_func_for_add_image(hotspot_image_added);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		hotspot_hook_control_center();
		hotspot_refresh();
	});
}

__attribute__((constructor))
static void
sbui_init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	volume_hud();
	power_menu();
	hotspot();
}
