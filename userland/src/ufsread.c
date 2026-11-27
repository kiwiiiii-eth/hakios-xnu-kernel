/*
 * How fast the phone's storage reads through the kernel's devices of its partitions (drivers/storage/hakios.ufs.c:
 * /dev/ufs_NAME, /dev/rufs_NAME, served by the loader on core 0), before a system is put there to be the root.
 * As /sbin/launchd of a small RAM disk: reads, says megabytes a second, and ends (the kernel then goes back to the
 * loader with its log).  Reads only.
 *
 *   ufsread_dev=NAME   the device, default rufs_userdata
 *   ufsread_mb=N       megabytes from its start, default 256
 */
#include "mini.h"

static const char *
boot_arg(const char *name)
{
	static char args[1024], value[128];
	size_t len = sizeof(args) - 1, n = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return NULL;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = args; *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, name, n) == 0 && p[n] == '=') {
			size_t i = 0;

			for (p += n + 1; *p && *p != ' ' && i < sizeof(value) - 1; p++) {
				value[i++] = *p;
			}
			value[i] = 0;
			return value;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return NULL;
}

static char buffer[1 << 20] __attribute__((aligned(4096)));

int
main(int argc, char **argv, char **envp)
{
	const char *arg;
	static char path[160];
	struct timeval t0, t1;
	unsigned megabytes = 0, done = 0, hash = 2166136261u;
	long ms;
	int fd = open("/dev/console", 2, 0);

	(void)argc, (void)argv, (void)envp;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	/* (boot_arg() answers in one buffer: each is used before the next is asked for) */
	arg = boot_arg("ufsread_mb");
	for (const char *p = arg != NULL ? arg : "256"; *p >= '0' && *p <= '9'; p++) {
		megabytes = megabytes * 10 + (unsigned)(*p - '0');
	}
	arg = boot_arg("ufsread_dev");
	strcpy(path, "/dev/");
	strcpy(path + 5, arg != NULL ? arg : "rufs_userdata");
	fd = open(path, 0, 0);
	if (fd < 0) {
		printf("ufsread: %s: %s\n", path, strerror(errno));
		return 1;
	}
	gettimeofday(&t0, NULL);
	while (done < megabytes) {
		ssize_t n = read(fd, buffer, sizeof(buffer));

		if (n != (ssize_t)sizeof(buffer)) {
			printf("ufsread: %s: megabyte %d: read gave %d (%s)\n", path, (int)done, (int)n, strerror(errno));
			break;
		}
		for (unsigned i = 0; i < sizeof(buffer); i += 4096) {
			hash = (hash ^ (unsigned char)buffer[i]) * 16777619u;   /* (a byte of each block: that it was read at all) */
		}
		done++;
	}
	gettimeofday(&t1, NULL);
	ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 + ((long)t1.tv_usec - (long)t0.tv_usec) / 1000;
	printf("ufsread: %s: %d MB in %d ms, %d MB/s (hash %x)\n", path, (int)done, (int)ms,
	    ms > 0 ? (int)((long)done * 1000 / ms) : 0, hash);
	return 0;
}
