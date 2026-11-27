/*
 * libhakios.dylib: code of ours inside stock Darwin programs.
 *
 * launchd starts the jobs named in INJECT (see build.sh / mkjobs.py) with
 * DYLD_INSERT_LIBRARIES pointing here; dyld honours that because the kernel's AMFI stand-in
 * says so. What is done depends on the program:
 *
 *   backboardd   the window server (CoreAnimation in QuartzCore). There is no GPU, so the
 *                render server must use CoreAnimation's own software rasterizer, and we want to
 *                see why a frame is or is not drawn. The C++ objects are reached by replacing
 *                entries of their virtual tables.
 *                It is also where input enters the system: backboardd is the HID event server,
 *                and without Apple's multitouch driver it has no touch screen. We give it one:
 *                a "virtual" HID service that turns the samples of /dev/hakios-touch into the
 *                events that driver would have sent.
 *
 *                And it does not wait five seconds for an ambient light sensor.
 *
 *   containermanagerd (both of them)
 *                They ask the user manager for the "personas" of the user, of which this
 *                system has none, twenty times a second apart before they give up -- and
 *                everything that needs a container (the window server, for one) waits with
 *                them. Told that personas are not supported, they do not ask.
 *
 * Addresses of things inside the dyld shared cache are those of Darwin userland 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "hakios.gl.h"
#include "hakios.input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hakios: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: CA::WindowServer ------------------------------------------------------------ */

#define VT_APPLE_CLCD_DISPLAY   0x1d5b44520UL   /* vtable for CA::WindowServer::AppleCLCDDisplay */
#define VT_EAGL_SERVER          0x1d5b45c58UL   /* vtable for CA::WindowServer::EAGLServer */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

#define DISPLAY_FLAGS(display)  (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display)   (*(uint8_t *)((char *)(display) + 0x233))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		say("hakios: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
		    (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	return ready;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int ignore = orig_ignore_update_p(display, update) & 1;

	if (ignore != last_ignore || calls_ignore < 3) {
		uint64_t *q = (uint64_t *)((char *)display + 0x220);

		say("hakios: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("hakios:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
		    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
		    (unsigned long long)q[5]);
		last_ignore = ignore;
	}
	calls_ignore++;
	return ignore;
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		say("hakios: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("hakios: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while there is none it tries again to make a
 * Metal and then an OpenGL ES context, which costs a lot and cannot succeed here. One try is enough:
 * without a renderer the server draws with CA::WindowServer::Server::sw_renderer().
 */
static void *
my_renderer(void *server)
{
	static int failed, said;
	void *renderer;

	if (failed) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		failed = 1;
		say("hakios: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("hakios: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hakios: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each: an
 * image as it is, an image through a colour matrix, a blur of so many samples, ...), and asks
 * its renderer which of them it has. Its OpenGL ES renderer has not half of them (the Metal one
 * has all). Which ones are asked for and missing is worth knowing: said once each.
 * (The last of the ninety, "cst", is not asked about: the OpenGL ES renderer draws it with no
 * program at all. The lock screen's wallpaper goes that way; see plain_draw_begin() in eagl.c.)
 */
#define VT_GLES_CONTEXT         0x1d5b3b540UL   /* vtable for CA::OGL::GLESContext */

static int gl_loaded;                           /* CoreAnimation draws with OpenGL ES (opengl_es() below) */
static void (*gl_function_refused)(unsigned function);  /* our OpenGL ES library keeps count (eagl.c, the census) */
static int (*orig_function_supported)(void *context);

/*
 * Boot-arg glyes=N[,N...]: the renderer is made to answer that it has those functions after all.
 * It has no shader for them, so CoreAnimation draws them with no program -- which is the way the
 * functions it never asks about are drawn, and which our OpenGL ES library stands in for
 * (eagl.c, plain_draw_begin()); without this, what needs such a function is not drawn at all.
 * For finding out what a missing function is for: with it the draw is made, its numbers arrive
 * (function_uniform below), and the picture says whether the thing was that function's.
 */
/*
 * Saying yes to a function the renderer has no shader for is not enough and not safe. Told yes for
 * "sb3" (40) -- the four samples of a shadow's blur, which our OpenGL ES library has a stand-in for
 * (eagl.c, blur4_fragment) -- CoreAnimation takes the way of one it has a shader for, finds none,
 * and the window server dies; it came back every half a minute, with SpringBoard after it, and the
 * stand-in was never reached: a function it asks about and is told yes to is not one it draws with
 * no program. What is needed is to hand it a program where it looks for one, which is another
 * entry of the same table. Until then it is told the truth, and what needs that function is not
 * drawn -- the shadow under a card of the switcher of running applications.
 */
/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		lr = (uintptr_t)arm_thread_state64_get_lr(context->uc_mcontext->__ss);
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)arm_thread_state64_get_fp(context->uc_mcontext->__ss) : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("hakios: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* An Objective-C exception that nobody catches ends in abort(), not in a fault, and a program
	   that dies that way says nothing at all here -- a run that restarted a daemon 31 times was
	   silent until these three were watched as well. */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/*
 * CoreAnimation's OpenGL ES renderer keeps the source of one fragment shader per function in a
 * table of its own -- unslid 0x1d5b54790, indexed by the function's number -- and the functions it
 * has no shader for are null entries there. That is the whole of "the renderer has no function
 * 40": GLContext::create_shader takes the source from that table, and function_supported answers
 * from the same place. Made to answer yes without the entry, create_shader hands the null to
 * strdup and the window server dies in strlen, which is what glyes=40 did for a whole afternoon.
 *
 * So the way to give the renderer a function is to give it a source. "sb3" (40) is the single
 * pass of a blur -- GaussianBlurFilter::render -> Context::blur_surface ->
 * BlurState::single_pass_blur -- the one that lays the shadow under a card of the switcher of
 * running applications. It is given the same four places as "ds4" (26) and has no weights of its
 * own, so this is ds4's own shader with the weights taken out, in the dialect the others are
 * written in (sampler2DRect, varying, gl_FragData -- the preamble is added by the renderer).
 *
 * Where this stands. With the entry filled the window server no longer dies: thirty-odd crashes a
 * run become none, with glyes=40 as well, and the function really is drawn -- the renderer rewrites
 * our source into its own dialect (sampler2DRect becomes sampler2D) and compiles it, and the
 * picture changes by a level or two across the whole screen where before it did not change at all.
 *
 * What it does not do is bring back a shadow, and the reason is that none was missing. This
 * function was taken up because a note here said the shadow under a card of the switcher was not
 * drawn; that note was reasoning from "the function is refused", not an observation. Against the
 * same frame drawn by CoreAnimation's software renderer -- which draws everything correctly, only
 * slowly -- the edge of a card is the same to the level: 18, 17, 17, then the card. There is no
 * soft falloff there in either, so there is nothing of that kind to restore.
 *
 * Its effect is nil for a second reason worth writing down: the four places reach the shader as
 * varyings that a vertex shader computes from "uniform highp vec2 offset[4]", and the two groups
 * of four numbers function_uniform delivers for it are of the order of 5e-19 -- zero, for a
 * blur. CoreAnimation asks for the single-pass blur exactly when the radius is too small to need
 * the multi-pass one, so the four samples coincide and the pass copies what it was given.
 */
#define CA_SHADER_TABLE         0x1d5b54790UL

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if (*entry != 0) {
		say("hakios: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("hakios: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("hakios: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("hakios: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
}

/*
 * CA::OGL::Context::compile_shader(const char *) -- slot 0x060 of the same table. The OpenGL ES
 * renderer does not carry its shaders ready-made: it assembles the text of each from a preamble
 * (GLESContext::shader_preamble, slot 0x400) and pieces chosen by what the draw needs, and
 * compiles it here. That makes this the place where it looks for a program, and therefore the
 * place where one can be handed to it for a function it has no pieces for ("sb3", 40: the four
 * samples of a shadow's blur, missing under the cards of the switcher). This writes down what it
 * is asked to compile, so that a run with glyes=40 says whether a source is assembled at all
 * before the window server dies, and what it looks like if so.
 */
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

static uintptr_t
my_compile_shader(void *context, const char *source)
{
	static unsigned told, seen;

	/* that it is called at all is worth one line, because a renderer that compiles nothing here
	   would mean the text is assembled and compiled somewhere this table does not reach */
	if (seen++ == 0) {
		say("hakios: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("hakios: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("hakios:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("hakios: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("hakios: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
		    function, function < 90 ? names + 4 * function : "?");
	}
	return supported;
}

/*
 * A function that takes numbers besides its textures (a colour matrix, say) is given them through
 * GLContext::function_uniform(), which hands them to the function's shader. The functions the
 * OpenGL ES renderer has no shader for are drawn all the same, with no program, and their numbers
 * go nowhere. Our OpenGL ES library draws some of those with programs of its own (eagl.c,
 * plain_draw_begin()): it is told the numbers here, and with them the context, in which it
 * finds which function a draw is.
 */
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	/*
	 * Who is calling. Told yes for a function it has no shader for (glyes=40), CoreAnimation gets
	 * this far -- it hands the function its numbers -- and then dies in whatever comes next, which
	 * is where it takes the program for the function out of a table of its own. These addresses,
	 * unslid, name that caller for "ipsw dyld a2s", and it is the only thread to pull: nothing is
	 * ever compiled for the function (no shader arrives at our library), and the compiler is not
	 * reached through the renderer's table either.
	 */
	if (say_yes[function < 90 ? function : 0]) {
		static uint8_t told[90];

		if (function < 90 && !told[function]) {
			told[function] = 1;
			say("hakios: function %u takes its numbers from %#lx %#lx %#lx\n", function,
			    (uintptr_t)__builtin_return_address(0) - slide,
			    (uintptr_t)__builtin_return_address(1) - slide,
			    (uintptr_t)__builtin_return_address(2) - slide);
		}
	}
	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
}

static void
window_server_fixes(void)
{
	orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
	orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
	    my_ignore_update_p);
	orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
	    my_begin_update);
	orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
	    my_finish_update);
	orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
	orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
	    my_render_update);
	if (gl_loaded) {
		orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
		    my_function_supported);
		if (gl_function_values != NULL) {
			orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
			    my_function_uniform);
		}
		/* always, because the boot-args are read in another place than this and the order of the
		   two is not ours to rely on; the hook writes nothing unless glcompile=1 */
		orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
		    VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
		say("hakios: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("hakios: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
	    orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for the ambient light sensor to turn up among the HID services. There is none
 * and none will come: do not wait for that.
 *
 * Wait for something else instead, at this convenient point: our own touch screen service.
 * backboardd takes for its touch screen a service that is there when its touch processing
 * starts, a moment later; one that turns up afterwards it does not notice. (For as long as
 * the five seconds were waited here, ours was always in time by accident.)
 */
static dispatch_semaphore_t input_services_made;

static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	if (input_services_made != NULL &&
	    dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
		say("hakios: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("hakios: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on Darwin (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static bool service_external;   /* make_service(): a Bluetooth device's, not built in (hid_thread()) */

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), service_external ? kCFBooleanFalse : kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), service_external ? CFSTR("Bluetooth Low Energy") : CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("hakios: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("redwood input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("hakios: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
		screen = make_service(client, "HakiOS touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("hakios: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "HakiOS buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	say("hakios: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (count++ < 4) {
			say("hakios: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	return NULL;
}

/*
 * Bluetooth keyboards (the 6T, run 91: an ESP32 as a BLE keyboard connected and typed nothing). bluetoothd makes a HID
 * device of one through IOHIDResource, which the kernel stands in for (drivers/input/hakios.hidresource.cpp) and queues
 * what it is handed on /dev/hakios-hid: a device made (its report descriptor), its input reports, a device gone. Here
 * each keyboard report -- the boot keyboard's layout: modifiers, a reserved byte, six keys, after the report id if the
 * descriptor has ids -- is compared with the one before, and the keys that went down or up are posted as keyboard events
 * (page 7) through a virtual service of a keyboard that is not built in.
 */
#define HAKIOS_HID_DEVICE       "/dev/hakios-hid"
#define HID_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_KEYBOARD      0x06
#define HID_PAGE_KEYBOARD       0x07

struct hid_record {
	uint16_t type, device;
	uint32_t length;
};

/* the report id of the keyboard collection (Generic Desktop / Keyboard), 0 if the descriptor has no ids, -1 if no keyboard */
static int
keyboard_report_id(const uint8_t *d, uint32_t length)
{
	uint32_t page = 0;
	bool in_keyboard = false, any_id = false;

	for (uint32_t i = 0; i < length;) {
		uint8_t prefix = d[i], size = prefix & 3;
		uint32_t value = 0;

		if (prefix == 0xfe) {           /* a long item */
			i += 3 + (i + 1 < length ? d[i + 1] : 0);
			continue;
		}
		size = size == 3 ? 4 : size;
		for (uint8_t b = 0; b < size && i + 1 + b < length; b++) {
			value |= (uint32_t)d[i + 1 + b] << (8 * b);
		}
		switch (prefix & 0xfc) {
		case 0x04: page = value; break;                                 /* Usage Page */
		case 0x08: if (page == HID_PAGE_GENERIC_DESKTOP && value == HID_USAGE_KEYBOARD) { in_keyboard = true; } break;
		case 0x84:                                                      /* Report ID */
			any_id = true;
			if (in_keyboard) {
				return (int)value;
			}
			break;
		}
		i += 1 + size;
	}
	return in_keyboard ? (any_id ? -1 : 0) : -1;
}

static void
send_key(IOHIDServiceClientRef keyboard, uint32_t usage, bool down)
{
	IOHIDEventRef event = IOHIDEventCreateKeyboardEvent(NULL, mach_absolute_time(), HID_PAGE_KEYBOARD, usage, down, 0);

	if (event != NULL) {
		IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 0);
		IOHIDVirtualServiceClientDispatchEvent(keyboard, event);
		CFRelease(event);
	}
}

static void *
hid_thread(__unused void *arg)
{
	static uint8_t buffer[8192];
	IOHIDEventSystemClientRef client;
	IOHIDServiceClientRef keyboard = NULL;
	uint8_t held[8] = { 0 };                /* the modifiers, a reserved byte, six keys: the last report */
	int report_id = -1, device = -1;
	unsigned said = 0;

	pthread_setname_np("hakios bluetooth keys");
	for (int tries = 0; tries < 120 && (device = open(HAKIOS_HID_DEVICE, O_RDONLY)) < 0; tries++) {
		sleep(1);
	}
	if (device < 0) {
		say("hakios: keys: %s: %s\n", HAKIOS_HID_DEVICE, strerror(errno));
		return NULL;
	}
	if (input_services_made != NULL) {
		dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
		dispatch_semaphore_signal(input_services_made);         /* (for whoever else waits on it) */
	}
	client = IOHIDEventSystemClientCreateWithType(NULL, HID_CLIENT_SIMPLE, NULL);
	if (client == NULL) {
		say("hakios: keys: no HID client\n");
		return NULL;
	}
	IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("hakios.keys", NULL));
	for (;;) {
		ssize_t got = read(device, buffer, sizeof(buffer));

		if (got <= 0) {
			if (got < 0 && errno == EINTR) {
				continue;
			}
			say("hakios: keys: reading %s: %s\n", HAKIOS_HID_DEVICE, got < 0 ? strerror(errno) : "end");
			break;
		}
		for (ssize_t at = 0; at + (ssize_t)sizeof(struct hid_record) <= got;) {
			struct hid_record r;
			const uint8_t *data = buffer + at + sizeof(r);

			memcpy(&r, buffer + at, sizeof(r));
			at += (ssize_t)sizeof(r) + r.length;
			if (at > got) {
				break;
			}
			if (r.type == 1) {
				report_id = keyboard_report_id(data, r.length);
				say("hakios: keys: device %u: a %u-byte report descriptor; the keyboard's report id %d (-1: no keyboard)\n",
				    r.device, r.length, report_id);
				if (report_id >= 0 && keyboard == NULL) {
					service_external = true;
					keyboard = make_service(client, "Bluetooth keyboard", HID_PAGE_GENERIC_DESKTOP, HID_USAGE_KEYBOARD, false);
					service_external = false;
					say("hakios: keys: the keyboard service %p\n", (void *)keyboard);
				}
				memset(held, 0, sizeof(held));
			} else if (r.type == 2 && keyboard != NULL && report_id >= 0) {
				const uint8_t *k = data;
				uint32_t n = r.length;

				if (report_id > 0) {
					if (n < 1 || k[0] != report_id) {
						continue;               /* another collection's report (media keys and the like) */
					}
					k++;
					n--;
				}
				if (n < 8) {
					continue;
				}
				if (said++ < 6) {
					say("hakios: keys: report %02x %02x %02x %02x %02x %02x %02x %02x\n", k[0], k[1], k[2], k[3], k[4], k[5],
					    k[6], k[7]);
				}
				for (int bit = 0; bit < 8; bit++) {     /* the modifiers: 0xe0 left control .. 0xe7 right GUI */
					bool was = held[0] & (1 << bit), is = k[0] & (1 << bit);

					if (was != is) {
						send_key(keyboard, 0xe0 + (uint32_t)bit, is);
					}
				}
				for (int i = 2; i < 8; i++) {           /* keys let go */
					if (held[i] > 1 && memchr(k + 2, held[i], 6) == NULL) {
						send_key(keyboard, held[i], false);
					}
				}
				for (int i = 2; i < 8; i++) {           /* keys pressed */
					if (k[i] > 1 && memchr(held + 2, k[i], 6) == NULL) {
						send_key(keyboard, k[i], true);
					}
				}
				memcpy(held, k, 8);
			} else if (r.type == 3) {
				if (keyboard != NULL) {
					for (int bit = 0; bit < 8; bit++) {
						if (held[0] & (1 << bit)) {
							send_key(keyboard, 0xe0 + (uint32_t)bit, false);
						}
					}
					for (int i = 2; i < 8; i++) {
						if (held[i] > 1) {
							send_key(keyboard, held[i], false);
						}
					}
				}
				memset(held, 0, sizeof(held));
				say("hakios: keys: device %u gone\n", r.device);
			}
		}
	}
	close(device);
	return NULL;
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
	if (pthread_create(&thread, NULL, hid_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
		say("hakios: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("hakios: container manager: no personas\n");
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1), which the service HakiosGPU says (it is there
 * before the first program runs, or not at all). Everywhere else -- QEMU, a run without the
 * boot-arg, a phone without the GPU's firmware in the image, a GPU that did not start --
 * nothing is loaded, and CoreAnimation draws with its own software renderer as in an image
 * without the library.
 * Boot-arg gl=0: not even with a GPU; gl=1: also without one, by Mesa's software driver (slow;
 * for working on eagl.c in QEMU).
 */
static void
opengl_es(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;
	void *library;
	int wanted = -1, told = 0;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return;                                 /* (an image built without it) */
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		functions_said_yes(arguments);
		compile_shaders_said = strstr(arguments, "glcompile=1") != NULL;
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				wanted = argument[3] != '0';
				told = 1;
			}
		}
	}
	if (wanted < 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
		wanted = gpu != 0;
		if (gpu != 0) {
			IOObjectRelease(gpu);
		}
	}
	if (!wanted) {
		say("hakios: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	if ((library = dlopen(HAKIOS_GL, RTLD_NOW)) == NULL) {
		say("hakios: %s: %s\n", HAKIOS_GL, dlerror());
		return;
	}
	gl_loaded = 1;
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
	    "hakios_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(library, "hakios_gl_function_refused");
}

/*
 * What CoreAnimation's Metal renderer looks for when it starts (CA::OGL::MetalContext asserts metallib_url != nil:
 * this image, made for the iPhone X, has no default.metallib in QuartzCore.framework, only default.pipelinelib).
 * Every NSBundle lookup of a "metallib" or "pipelinelib" resource in the window server is said, with what it found;
 * boot-arg ca_metallib=PATH answers a lookup of default.metallib that found nothing with PATH.
 */
static id (*orig_url_for)(id self, SEL selector, id name, id extension);
static id (*orig_url_for_in)(id self, SEL selector, id name, id extension, id subdirectory);
static const char *ca_metallib;
static unsigned lookups_said;

static const char *
utf8(id string)
{
	return string != NULL ? ((const char *(*)(id, SEL))objc_msgSend)(string, sel_registerName("UTF8String")) : "(nil)";
}

/* ---- backboardd: the brightness slider, to the OnePlus 6T's panel ------------------------------------ */

/*
 * CoreBrightness (in backboardd) is told the brightness the user wants -- Control Center's slider, Settings --
 * through -setProperty:forKey: of its brightness system, and on an iPhone it then sets the backlight's IOKit service.
 * There is none here; on the OnePlus 6T the kernel's DSIBL driver sends the panel its brightness instead (sysctl
 * kern.hakios.brightness, 0..1023). So the property "DisplayBrightness" (0..1, the slider's own scale) is caught on its
 * way in, mapped to the panel's (gamma 2.2: the slider is perceptual, the panel's value linear in light), and handed
 * to the kernel; the call goes on as it was. The first calls of every key are said in the log, to learn the protocol.
 * Nothing happens on a kernel without the sysctl (it fails quietly after one line).
 */
static IMP orig_bs_set_property[2];
static unsigned bs_said, bs_last = 0xffffffffu, bs_got_said;
static double bs_level = -1;          /* the last level asked for, 0..1; -1 before any */
static IMP orig_bs_copy_property[2];

static double
bs_number(CFTypeRef value, bool *found)
{
	double d = 0;

	*found = false;
	if (value == NULL) {
		return 0;
	}
	if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		*found = CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &d);
	} else if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		static const CFStringRef keys[] = { CFSTR("Brightness"), CFSTR("DisplayBrightness"), CFSTR("Value") };

		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]) && !*found; i++) {
			CFTypeRef inner = CFDictionaryGetValue((CFDictionaryRef)value, keys[i]);

			if (inner != NULL && CFGetTypeID(inner) == CFNumberGetTypeID()) {
				*found = CFNumberGetValue((CFNumberRef)inner, kCFNumberDoubleType, &d);
			} else if (inner != NULL && CFGetTypeID(inner) == CFStringGetTypeID()) {
				d = CFStringGetDoubleValue((CFStringRef)inner);   /* (Control Center's slider: {Brightness = "0.50"; Commit = 0}) */
				*found = true;
			}
		}
	}
	return d;
}

static void
bs_set_property(id self, SEL selector, id value, id key, unsigned which)
{
	char name[64] = { 0 };

	if (key != NULL && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)key, name, sizeof(name), kCFStringEncodingUTF8);
	}
	if (bs_said < 40) {
		bs_said++;
		say("hakios: brightness: %s %s = %s\n", which ? "BrightnessSystem" : "BrightnessSystemClient", name,
		    value != NULL ? utf8(((id (*)(id, SEL))objc_msgSend)(value, sel_registerName("description"))) : "(nil)");
	}
	if (strcmp(name, "DisplayBrightness") == 0) {
		bool found;
		double level = bs_number((CFTypeRef)value, &found);

		if (found) {
			int raw;

			level = level < 0 ? 0 : level > 1 ? 1 : level;
			bs_level = level;
			raw = (int)(1023.0 * pow(level, 2.2) + 0.5);
			raw = raw < 4 ? 4 : raw;
			if ((unsigned)raw != bs_last) {
				if (sysctlbyname("kern.hakios.brightness", NULL, NULL, &raw, sizeof(raw)) == 0) {
					bs_last = (unsigned)raw;
				} else if (bs_last == 0xffffffffu) {
					say("hakios: brightness: the kernel takes none (kern.hakios.brightness: %s)\n", strerror(errno));
					bs_last = 0xfffffffeu;
				}
			}
		}
	}
	((void (*)(id, SEL, id, id))orig_bs_set_property[which])(self, selector, value, key);
}

static void
bs_set_property_client(id self, SEL selector, id value, id key)
{
	bs_set_property(self, selector, value, key, 0);
}

static void
bs_set_property_system(id self, SEL selector, id value, id key)
{
	bs_set_property(self, selector, value, key, 1);
}

/*
 * ... and on its way out: a slider that asked for a level was pulled back to the old one when it let go (11-11), the
 * brightness system answering from what it believes the (absent) backlight to be. Once a level has been asked for,
 * "DisplayBrightness" answers it, in the shape the original answer had (a number, or a dictionary whose "Brightness"
 * is set). The first answers of every key are said in the log.
 */
static id
bs_copy_property(id self, SEL selector, id key, unsigned which)
{
	id answer = ((id (*)(id, SEL, id))orig_bs_copy_property[which])(self, selector, key);
	char name[64] = { 0 };

	if (key != NULL && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)key, name, sizeof(name), kCFStringEncodingUTF8);
	}
	if (bs_got_said < 30) {
		bs_got_said++;
		say("hakios: brightness: %s gives %s = %s\n", which ? "BrightnessSystem" : "BrightnessSystemClient", name,
		    answer != NULL ? utf8(((id (*)(id, SEL))objc_msgSend)(answer, sel_registerName("description"))) : "(nil)");
	}
	if (strcmp(name, "CBBrightnessControlAvailable") == 0 && answer == NULL) {
		/* Control Center asks this before it lets its slider be moved: with no backlight there is no answer (11-11) */
		return (id)CFRetain(kCFBooleanTrue);
	}
	if (bs_level >= 0 && strcmp(name, "DisplayBrightness") == 0 && answer != NULL) {
		id number = ((id (*)(id, SEL, double))objc_msgSend)((id)objc_getClass("NSNumber"), sel_registerName("numberWithDouble:"),
		    bs_level);

		if (CFGetTypeID((CFTypeRef)answer) == CFNumberGetTypeID()) {
			((void (*)(id, SEL))objc_msgSend)(answer, sel_registerName("release"));
			answer = ((id (*)(id, SEL))objc_msgSend)(number, sel_registerName("retain"));
		} else if (CFGetTypeID((CFTypeRef)answer) == CFDictionaryGetTypeID()) {
			id copy = ((id (*)(id, SEL))objc_msgSend)(answer, sel_registerName("mutableCopy"));

			((void (*)(id, SEL, id, id))objc_msgSend)(copy, sel_registerName("setObject:forKey:"), number,
			    (id)CFSTR("Brightness"));
			((void (*)(id, SEL))objc_msgSend)(answer, sel_registerName("release"));
			answer = copy;
		}
	}
	return answer;
}

static id
bs_copy_property_client(id self, SEL selector, id key)
{
	return bs_copy_property(self, selector, key, 0);
}

static id
bs_copy_property_system(id self, SEL selector, id key)
{
	return bs_copy_property(self, selector, key, 1);
}

static void
brightness_bridge(void)
{
	/* CoreBrightness is loaded a moment after the start: look for its classes for half a minute */
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		static const char *const classes[2] = { "BrightnessSystemClient", "BrightnessSystem" };
		static const IMP ours[2] = { (IMP)bs_set_property_client, (IMP)bs_set_property_system };
		static const IMP ours_get[2] = { (IMP)bs_copy_property_client, (IMP)bs_copy_property_system };
		bool done[2] = { false, false };

		for (int tries = 0; tries < 60 && !(done[0] && done[1]); tries++) {
			for (unsigned i = 0; i < 2; i++) {
				Class cls = done[i] ? NULL : objc_getClass(classes[i]);
				Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setProperty:forKey:")) : NULL;

				if (method != NULL) {
					Method getter = class_getInstanceMethod(cls, sel_registerName("copyPropertyForKey:"));

					orig_bs_set_property[i] = method_setImplementation(method, ours[i]);
					if (getter != NULL) {
						orig_bs_copy_property[i] = method_setImplementation(getter, ours_get[i]);
					}
					done[i] = true;
					say("hakios: brightness: -[%s setProperty:forKey:] caught%s\n", classes[i],
					    getter != NULL ? ", and -copyPropertyForKey:" : "");
				}
			}
			usleep(500000);
		}
		if (!done[0] && !done[1]) {
			say("hakios: brightness: no brightness system found\n");
		}
	});
}

static id
metallib_answer(id self, id name, id extension, id found)
{
	const char *ext = extension != NULL ? utf8(extension) : "";

	if (strstr(ext, "metallib") == NULL && strstr(ext, "pipelinelib") == NULL) {
		return found;
	}
	if (lookups_said++ < 40) {
		id path = found != NULL ? ((id (*)(id, SEL))objc_msgSend)(found, sel_registerName("path")) : NULL;

		say("hakios: %s asks bundle %s for %s.%s: %s\n", getprogname(),
		    utf8(((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("bundlePath"))), utf8(name), ext,
		    found != NULL ? utf8(path) : "nothing");
	}
	if (found == NULL && ca_metallib != NULL && name != NULL && strcmp(utf8(name), "default") == 0 &&
	    strcmp(ext, "metallib") == 0) {
		id text = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), ca_metallib);

		return ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSURL"), sel_registerName("fileURLWithPath:"), text);
	}
	return found;
}

static id
my_url_for(id self, SEL selector, id name, id extension)
{
	return metallib_answer(self, name, extension, orig_url_for(self, selector, name, extension));
}

static id
my_url_for_in(id self, SEL selector, id name, id extension, id subdirectory)
{
	return metallib_answer(self, name, extension, orig_url_for_in(self, selector, name, extension, subdirectory));
}

static void
watch_metallib_lookups(const char *arguments)
{
	static char path[256];
	const char *at = strstr(arguments, "ca_metallib=");
	Class bundle = objc_getClass("NSBundle");
	Method two, three;

	if (at != NULL) {
		size_t length = strcspn(at + 12, " ");

		if (length < sizeof(path)) {
			memcpy(path, at + 12, length);
			ca_metallib = path;
		}
	}
	if (bundle == NULL) {
		return;
	}
	if ((two = class_getInstanceMethod(bundle, sel_registerName("URLForResource:withExtension:"))) != NULL) {
		orig_url_for = (id (*)(id, SEL, id, id))method_setImplementation(two, (IMP)my_url_for);
	}
	if ((three = class_getInstanceMethod(bundle, sel_registerName("URLForResource:withExtension:subdirectory:"))) != NULL) {
		orig_url_for_in = (id (*)(id, SEL, id, id, id))method_setImplementation(three, (IMP)my_url_for_in);
	}
	say("hakios: metallib lookups in %s are said%s%s\n", getprogname(), ca_metallib ? "; default.metallib is " : "",
	    ca_metallib ? ca_metallib : "");
}

#ifdef HAKIOS_DARWIN25
/* ---- backboardd, Darwin 25: CoreAnimation's OpenGL ES renderer in the window server ---------------- */

/*
 * Darwin 25's window server draws with Metal or, where there is no Metal device, with CoreAnimation's software
 * rasterizer; it no longer tries OpenGL ES in between as 16.5's did (CA::WindowServer::EAGLServer::renderer()). But
 * the OpenGL ES renderer is all still there -- CA::OGL::GLESContext, for CARenderer on an EAGLContext -- and it is made
 * the way it always was: from an EAGLContext, whose -getMacroContextPrivate gives it its table of functions. So where
 * the kernel has the phone's GPU (the service HakiosGPU) the window server is given one: an EAGLContext of ours
 * (libhakios.gl.dylib: src/eagl.c, Mesa behind it), a GLESContext on it by QuartzCore's own gles_new_context(), and
 * the rest as CA::WindowServer::AccelServer::renderer() does it for a Metal context, whose place in the classes'
 * tables of virtual functions this takes.
 *
 * The addresses are those of QuartzCore in the shared cache of userland 26.5.2 (23F84) for iPhone12,1, before the
 * slide; each function is looked at before it is used, and anything else than what is expected leaves the window
 * server with its software renderer. Boot-arg gl=0: not this at all.
 */
#define D25_ACCEL_RENDERER      0x183f35d3cUL   /* CA::WindowServer::AccelServer::renderer() */
#define D25_GLES_NEW_CONTEXT    0x18410426cUL   /* gles_new_context(void *, void *, unsigned) */
#define D25_UPDATE_COLOR_CACHE  0x1840d1620UL   /* CA::OGL::Context::update_color_program_cache() */
#define D25_FEATURE_FLAGS       0x1e9514e3cUL   /* global_feature_flags */
#define D25_MALLOC_ZONE         0x1ec5d25c0UL   /* malloc_zone (x_malloc_get_zone) */
#define D25_VTABLES_FROM        0x1ee1b1000UL   /* QuartzCore's tables of virtual functions lie between these */
#define D25_VTABLES_TO          0x1ee1c3000UL
#define D25_GL                  "/private/preboot/boot/lib/libhakios.gl.dylib"

#define SERVER_FLAGS(server)    (*(uint32_t *)((char *)(server) + 0xb8))
#define SERVER_DISPLAY(server)  (*(void **)((char *)(server) + 0x60))
#define SERVER_CONTEXT(server)  (*(void **)((char *)(server) + 0x5c8))
#define SERVER_RENDERER(server) (*(void **)((char *)(server) + 0x5d0))
#define DISPLAY_RENDERER(d)     (*(void **)((char *)(d) + 0x148))
#define CONTEXT_FLAGS(context)  (*(uint32_t *)((char *)(context) + 0x280))
#define CONTEXT_FEATURES(c)     (*(uint32_t *)((char *)(c) + 0x378))

extern void *malloc_zone_calloc(void *zone, size_t count, size_t size);
extern void *malloc_default_zone(void);

static void *
gles_renderer(void *server)
{
	static int tried;
	void *(*new_context)(void *, void *, unsigned) = (void *(*)(void *, void *, unsigned))(D25_GLES_NEW_CONTEXT + slide);
	void (*update_color_cache)(void *) = (void (*)(void *))(D25_UPDATE_COLOR_CACHE + slide);
	void *zone = *(void **)(D25_MALLOC_ZONE + slide);
	struct {
		uint32_t api, pad;
		id context;
	} request = { 3, 0, NULL };
	Class eagl_class = objc_getClass("EAGLContext");
	void *context, *display = SERVER_DISPLAY(server);
	void **renderer;
	uint32_t flags, before;
	int32_t zero = 0;
	id eagl, properties;

	if (SERVER_RENDERER(server) != NULL) {
		return SERVER_RENDERER(server);
	}
	if (tried) {
		return NULL;                    /* the server's own software renderer, then */
	}
	tried = 1;
	if (eagl_class == NULL || display == NULL) {
		say("hakios: gl: no EAGLContext class (%p) or no display (%p): the software renderer\n", eagl_class, display);
		return NULL;
	}
	properties = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableDictionary"), sel_registerName("new"));
	eagl = ((id (*)(id, SEL))objc_msgSend)((id)eagl_class, sel_registerName("alloc"));
	eagl = ((id (*)(id, SEL, unsigned long, id))objc_msgSend)(eagl, sel_registerName("initWithAPI:properties:"), 3, properties);
	if (eagl == NULL) {
		say("hakios: gl: no EAGLContext of API 3: the software renderer\n");
		return NULL;
	}
	((void (*)(id, SEL, unsigned, const int32_t *))objc_msgSend)(eagl, sel_registerName("setParameter:to:"), 608, &zero);
	request.context = eagl;
	context = new_context(&request, NULL, 0);
	if (context == NULL) {
		say("hakios: gl: gles_new_context() made nothing: the software renderer\n");
		return NULL;
	}
	/* (as AccelServer::renderer() makes its own: 0x68 bytes, the context, a buffer of 256 KB, a 1) */
	renderer = malloc_zone_calloc(zone != NULL ? zone : malloc_default_zone(), 1, 0x68);
	renderer[0] = context;
	renderer[6] = malloc(0x40000);
	*(uint32_t *)((char *)renderer + 0x60) = 1;
	SERVER_CONTEXT(server) = context;
	SERVER_RENDERER(server) = renderer;
	/* the context's 28th virtual function: whether the framebuffer is compressed -- it is not */
	((void (*)(void *, int))(*(void ***)context)[0xe0 / 8])(context, 0);
	DISPLAY_RENDERER(display) = renderer;
	flags = SERVER_FLAGS(server) | 0x20010;         /* (what 16.5's EAGLServer gave its OpenGL ES context) */
	before = CONTEXT_FLAGS(context);
	CONTEXT_FLAGS(context) = flags;
	CONTEXT_FEATURES(context) = *(uint32_t *)(D25_FEATURE_FLAGS + slide) | flags;
	if ((before ^ flags) & 0x2000200) {
		update_color_cache(context);
	}
	say("hakios: gl: the window server draws with OpenGL ES: context %p, renderer %p, flags %#x\n", context, (void *)renderer, flags);
	return renderer;
}

static void
opengl_es_25(const char *arguments)
{
	static const uint32_t accel_expected[4] = { 0xd503237f, 0xa9be4ff4, 0xa9017bfd, 0x910043fd };
	static const uint32_t new_expected[3] = { 0xb9400008, 0x71000d1f, 0x54000301 };
	uintptr_t accel = D25_ACCEL_RENDERER + slide;
	io_service_t gpu;
	unsigned replaced = 0;

	for (const char *argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
		if ((argument == arguments || argument[-1] == ' ') && argument[3] == '0') {
			say("hakios: gl=0: CoreAnimation keeps its software renderer\n");
			return;
		}
	}
	if (access(D25_GL, R_OK) != 0) {
		return;                                 /* (an image without the library) */
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
	if (gpu == 0) {
		say("hakios: no GPU to draw with: CoreAnimation keeps its software renderer\n");
		return;
	}
	IOObjectRelease(gpu);
	if (memcmp((void *)accel, accel_expected, sizeof(accel_expected)) != 0 ||
	    memcmp((void *)(D25_GLES_NEW_CONTEXT + slide), new_expected, sizeof(new_expected)) != 0) {
		say("hakios: gl: this is not the QuartzCore of 23F84: CoreAnimation keeps its software renderer\n");
		return;
	}
	/* (EAGLContext is OpenGLES.framework's, which nothing in the window server has asked for yet) */
	if (dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW) == NULL || dlopen(D25_GL, RTLD_NOW) == NULL) {
		say("hakios: gl: %s\n", dlerror());
		return;
	}
	/*
	 * No compressed surfaces. The iPhone this system takes itself for has a GPU that draws into compressed surfaces
	 * ('&BGA') and a display that shows them; the window server with a renderer on a GPU asks for such, and the Adreno
	 * has no use for them ("-attachImage: ... an IOSurface of '&BGA': not done", and the window server aborts). Three
	 * questions CoreAnimation asks of the device, each answered once and kept in a byte: asked here first, then put right.
	 */
	{
		static const struct {
			const char *what;
			uintptr_t function, answer;
			uint32_t expected[2];
		} questions[] = {
			{ "framebuffer compression", 0x1841886c0UL, 0x1e9512648UL, { 0xd0329c48, 0xf9432108 } },
			{ "universal compression", 0x184188870UL, 0x1ec5cc8f8UL, { 0x90342228, 0xf944e908 } },
			{ "lossy universal compression", 0x1841888c8UL, 0x1ec5cc4c8UL, { 0x90342228, 0xf9439108 } },
		};

		/* (QuartzCore's own switch for it, force_disable_all_compression, which all three answers give way to) */
		*(volatile uint8_t *)(0x1e95154d1UL + slide) = 1;
		for (unsigned i = 0; i < sizeof(questions) / sizeof(questions[0]); i++) {
			int (*ask)(void) = (int (*)(void))(questions[i].function + slide);

			if (memcmp((void *)(questions[i].function + slide), questions[i].expected, sizeof(questions[i].expected)) != 0) {
				say("hakios: gl: %s: the function is not where it was: left alone\n", questions[i].what);
				continue;
			}
			if (ask()) {
				*(volatile uint8_t *)(questions[i].answer + slide) = 0;
				say("hakios: gl: %s was on, is %s\n", questions[i].what, ask() ? "on" : "off");
			}
		}
	}
	for (uintptr_t *slot = (uintptr_t *)(D25_VTABLES_FROM + slide); slot < (uintptr_t *)(D25_VTABLES_TO + slide); slot++) {
		if (*slot == accel) {
			vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);

			if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
				say("hakios: gl: the table at %p cannot be written\n", (void *)slot);
				continue;
			}
			*slot = (uintptr_t)gles_renderer;
			vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
			replaced++;
		}
	}
	say("hakios: gl: AccelServer::renderer() replaced in %u tables of virtual functions\n", replaced);
}
#endif /* HAKIOS_DARWIN25 */

__attribute__((constructor))
static void
hakios_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "backboardd") == 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		/*
		 * Boot-arg ca_vbl=1: CoreAnimation's frame clock from the display driver's refresh
		 * notifications. QuartzCore takes them (IOMobileFramebufferEnableVSyncNotifications) only on
		 * the phones with an A7 to A10, or when CA_FORCE_TIMER is in the environment
		 * (CADeviceUseVBL()); on the iPhone X it runs a timer of its own instead (CADisplayTimer),
		 * whose phase it would set from the frame timings of a display driver this one is not.
		 */
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "ca_vbl=1") != NULL) {
			setenv("CA_FORCE_TIMER", "1", 1);
			say("hakios: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		/*
		 * Boot-arg caenv=NAME=VALUE (as many as wanted): into the window server's environment, for
		 * CoreAnimation's own switches (CA_LOG_TRIPLE_BUFFERING=1, CA_FORCE_DOUBLE_BUFFERING=1 ...).
		 */
		for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *equals;

			if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
				continue;
			}
			memcpy(setting, argument + 6, length);
			setting[length] = 0;
			if ((equals = strchr(setting, '=')) != NULL) {
				*equals = 0;
				/* a fraction is written 0p25: the boot-arguments lose what follows a "." of a number (0.008 came as 0) */
				for (char *point = equals + 1; *point != 0; point++) {
					if (*point == 'p' && point > equals + 1 && point[-1] >= '0' && point[-1] <= '9' && point[1] >= '0' && point[1] <= '9') {
						*point = '.';
						break;
					}
				}
				setenv(setting, equals + 1, 1);
				say("hakios: caenv: %s=%s for the window server\n", setting, equals + 1);
			}
		}
		if (strstr(arguments, "metal_watch=1") != NULL || strstr(arguments, "ca_metallib=") != NULL) {
			watch_metallib_lookups(arguments);
		}
#ifndef HAKIOS_DARWIN25
		/*
		 * (Darwin 25, userland 26: none of these three. They reach into QuartzCore and BackBoardServices by addresses
		 * and selectors of 16.5's, and 26's window server draws with its software renderer and reaches the panel
		 * without them; what it lacks is the touch screen and the buttons, below, which IOKit's own functions give.)
		 */
		opengl_es();
		window_server_fixes();
		brightness_bridge();            /* (the OnePlus 6T: the slider to the panel, through kern.hakios.brightness) */
#endif
#ifdef HAKIOS_DARWIN25
		opengl_es_25(arguments);
#endif
		/*
		 * brightness_fixes() is not used for now: with it backboardd no longer took our touch
		 * screen service for its own ("still waiting for a main screen digitizer"), whether the
		 * service was made before or after the touch processing starts. (Most likely it only
		 * moved the start of the event server onto the moment make_service() now knows how to
		 * survive; not tried again since, and until it is, the five seconds stay.)
		 */
		input_start();
#ifndef HAKIOS_DARWIN25
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		/* (Darwin 25 has its personas: usermanagerd makes them, tools/ios26's init26 has it do so) */
		container_manager_fixes();
#endif
	}
}
