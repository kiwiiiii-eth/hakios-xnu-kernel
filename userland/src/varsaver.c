/*
 * varsaver: the launchd job that keeps /private/var across boots (com.hakios.varstore.plist), in place of
 * "var_store watch". It does what that did -- says to the kernel that it is the saver (kern.hakios.var_save = 1),
 * and when the kernel asks for a save as a run ends (2) has /redwood/bin/var_store save, then answers (3) -- and one
 * thing more: when the system itself goes down (Settings' Shut Down, Restart, launchctl reboot), launchd sends its jobs
 * SIGTERM before it calls reboot(), and var_store's own minimal libc has no signal handling. Here a SIGTERM is a
 * save, then the end. On a phone that boots by itself (no host to end the run) that is the only save there is, so
 * the plist gives launchd room to wait for it (ExitTimeOut: a save takes 50-60 s on the 6T), and a transaction is
 * kept open, without which launchd sends SIGKILL at a shutdown instead of SIGTERM.
 *
 *   usage: varsaver [SECONDS]      SECONDS: also save of its own accord that often (default: never)
 *
 * With no var_store=1 among the kernel's arguments it ends at once, as var_store watch did.
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* libxpc's (in libSystem), which the iPhoneOS SDK here has no header for */
void xpc_transaction_begin(void);

#define VAR_SAVE_IDLE   1
#define VAR_SAVE_WANTED 2
#define VAR_SAVE_DONE   3

extern char **environ;
static volatile sig_atomic_t terminating;

static void
on_term(int sig)
{
	(void)sig;
	terminating = 1;
}

static int
state_get(void)
{
	int value = 0;
	size_t size = sizeof(value);

	return sysctlbyname("kern.hakios.var_save", &value, &size, NULL, 0) == 0 ? value : -1;
}

static int
state_set(int value)
{
	return sysctlbyname("kern.hakios.var_save", NULL, NULL, &value, sizeof(value));
}

/* "NAME=VALUE" a whole word of the kernel's arguments */
static int
boot_arg_is(const char *name, const char *value)
{
	char arguments[1024] = "", want[64];
	size_t size = sizeof(arguments) - 1;
	char *at;

	snprintf(want, sizeof(want), "%s=%s", name, value);
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return 0;
	}
	for (at = strstr(arguments, want); at != NULL; at = strstr(at + 1, want)) {
		char after = at[strlen(want)];

		if ((at == arguments || at[-1] == ' ') && (after == 0 || after == ' ')) {
			return 1;
		}
	}
	return 0;
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static int
save(const char *why)
{
	char *argv[] = { (char *)"/redwood/bin/var_store", (char *)"save", NULL };
	double started = now();
	pid_t pid;
	int status = 0, error;

	printf("varsaver: %s: saving /private/var\n", why);
	if ((error = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ)) != 0) {
		printf("varsaver: %s: %s\n", argv[0], strerror(error));
		return 0;
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	printf("varsaver: %s in %.1f s\n", WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "saved" : "the save came to nothing",
	    now() - started);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int
main(int argc, char **argv)
{
	struct sigaction action;
	double every = argc > 1 ? atof(argv[1]) : 0, last;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!boot_arg_is("var_store", "1")) {
		printf("varsaver: not asked to keep the data volume (var_store=1): nothing to watch\n");
		return 0;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_term;
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	/*
	 * Open for as long as this runs: launchd SIGKILLs at shutdown a job (EnableTransactions) with none open, and only
	 * one with a transaction open is sent SIGTERM and given its ExitTimeOut (r163: killed at once, nothing saved).
	 */
	xpc_transaction_begin();
	if (state_set(VAR_SAVE_IDLE) != 0) {
		printf("varsaver: this kernel does not keep the data volume (%s)\n", strerror(errno));
		return 0;
	}
	printf("varsaver: watching: /private/var is saved when the run ends or the system goes down%s\n",
	    every > 0 ? ", and in between" : "");
	last = now();
	for (;;) {
		if (terminating) {
			return save("the system is going down (SIGTERM)") ? 0 : 1;
		}
		if (state_get() == VAR_SAVE_WANTED) {
			int ok = save("the kernel asks, as the run ends");

			/* answered either way: the kernel waits on this to go back to the loader */
			state_set(VAR_SAVE_DONE);
			return ok ? 0 : 1;
		}
		if (every > 0 && now() - last >= every) {
			save("of its own accord");
			last = now();
		}
		usleep(200000);
	}
}
