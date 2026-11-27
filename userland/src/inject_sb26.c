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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
}
