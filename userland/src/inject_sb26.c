/*
 * libhakios.sb.dylib for Darwin 25 (userland 26): code of ours inside SpringBoard. (userland/src/inject_sb.c is
 * 16.5's, and reaches most of what it changes by addresses of that system's; this begins again with what can be
 * found by name.)
 *
 * No setup assistant. A system that starts on an empty /private/var has never been set up, and SpringBoard shows the
 * "Hello" screen and then Setup -- which wants a language, a network and, in the end, an activation this phone has no
 * Secure Enclave to give. -[SBSetupManager _setSetupRequiredReason:] is where SpringBoard notes why setup is needed;
 * told 0 every time, it goes to its lock screen instead (as 16.5's does: inject_sb.c).
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
	va_list list;
	int n;

	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (n > 0) {
		write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

/* An instance method's implementation replaced, the one it had returned; NULL and a line when there is no such method. */
static IMP
replace_method(const char *class_name, const char *selector, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : class_getInstanceMethod(cls, sel_registerName(selector));

	if (method == NULL) {
		say("hakios: -[%s %s]: no such %s; left alone\n", class_name, selector, cls == NULL ? "class" : "method");
		return NULL;
	}
	return method_setImplementation(method, replacement);
}

static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * Activated. -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in its _state:
 * 2 = activated, 1 = not, 3 = unknown, and 0 while there is no answer -- there never is one here, where
 * mobileactivationd is not started (it wants a Secure Enclave). A phone that is not activated shows Settings and Phone
 * on its home screen and nothing else. (16.5's library does the same, at the same place: the ivar is found by its
 * name here, +8 in both systems.)
 */
#define LOCKDOWN_ACTIVATED      2
static ptrdiff_t lockdown_state_offset;

static void
setup_activation_state(id self, __unused SEL selector)
{
	*(int *)((char *)self + lockdown_state_offset) = LOCKDOWN_ACTIVATED;
}

/* What a class is made of, said once: for finding by name what 16.5's library found by address. (boot-arg sb_dump=CLASS) */
static void
dump_class(const char *class_name)
{
	Class cls = objc_getClass(class_name);
	unsigned count = 0;
	Ivar *ivars;
	Method *methods;

	if (cls == NULL) {
		say("hakios: no class %s\n", class_name);
		return;
	}
	ivars = class_copyIvarList(cls, &count);
	for (unsigned i = 0; i < count; i++) {
		say("hakios: %s ivar +%ld %s %s\n", class_name, (long)ivar_getOffset(ivars[i]), ivar_getName(ivars[i]),
		    ivar_getTypeEncoding(ivars[i]) ? ivar_getTypeEncoding(ivars[i]) : "?");
	}
	free(ivars);
	methods = class_copyMethodList(cls, &count);
	for (unsigned i = 0; i < count; i++) {
		say("hakios: %s -%s %s\n", class_name, sel_getName(method_getName(methods[i])),
		    method_getTypeEncoding(methods[i]) ? method_getTypeEncoding(methods[i]) : "?");
	}
	free(methods);
}

/*
 * -[LCSCaptureApplicationMonitor _beginObservingMetadataChanges]: watching the applications' records for changes, for the lock
 * screen's camera extensions. When one changes it gathers each capture extension's bundle and its container's into a set,
 * and an extension here has no container's identifier (nil: the applications of a first start are not installed, only named
 * -- init26): -[NSMutableSet addObject:nil] threw, and SpringBoard stopped (ios26-38, 43, 50, 54, 55: about 3 minutes after
 * the start; the second SpringBoard drew the home screen without its icons). Not watched: the lock screen's camera extensions
 * are as they were found when SpringBoard started.
 */
static void
no_metadata_watch(id self, SEL selector)
{
	(void)self;
	(void)selector;
}

__attribute__((constructor))
static void
hakios_sb_init(void)
{
	const char *name = getprogname();

	if (name == NULL || strcmp(name, "SpringBoard") != 0) {
		return;
	}
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", (IMP)set_setup_required_reason);
	say("hakios: SpringBoard: %s\n", original_set_setup_required_reason != NULL ? "no setup assistant" :
	    "the setup assistant is as it was");
	say("hakios: SpringBoard: %s\n", replace_method("LCSCaptureApplicationMonitor", "_beginObservingMetadataChanges",
	    (IMP)no_metadata_watch) != NULL ? "the lock screen's capture extensions are not watched for changes" :
	    "LCSCaptureApplicationMonitor not found: its watch is as it was");
	{
		Class lockdown = objc_getClass("SBLockdownManager");
		Ivar state = lockdown != NULL ? class_getInstanceVariable(lockdown, "_state") : NULL;
		const char *type = state != NULL ? ivar_getTypeEncoding(state) : NULL;

		if (state != NULL && type != NULL && strcmp(type, "i") == 0) {
			lockdown_state_offset = ivar_getOffset(state);
			say("hakios: SpringBoard: %s\n", replace_method("SBLockdownManager", "_setupActivationState",
			    (IMP)setup_activation_state) != NULL ? "activated, as far as SpringBoard asks" : "not activated");
		} else {
			say("hakios: SpringBoard: SBLockdownManager has no int _state: not activated\n");
		}
	}
	{
		static char arguments[1024];
		size_t size = sizeof(arguments) - 1;
		const char *dump;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && (dump = strstr(arguments, "sb_dump=")) != NULL) {
			char class_name[64];
			size_t length = strcspn(dump + 8, " ");

			if (length < sizeof(class_name)) {
				memcpy(class_name, dump + 8, length);
				class_name[length] = 0;
				dump_class(class_name);
			}
		}
	}
}
