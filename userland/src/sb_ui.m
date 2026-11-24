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
 * From the diagnostic adapters of 2026-11-23 (power_controls, ui_adapter), phone-tested in RAM runs.
 */
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
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

__attribute__((constructor))
static void
sbui_init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	volume_hud();
	power_menu();
}
