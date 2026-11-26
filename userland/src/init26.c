/*
 * /sbin/launchd of the small image a Darwin 25 (userland 26) system is started from under QEMU.
 *
 * The system is too big to be held in memory (its dyld shared cache alone is 5 GB), so it is a disk
 * of the host: an APFS image on /dev/rvblk0 (boot-arg virtio_blk=1; qemu/run-ios26.sh).  This program
 * is what the kernel runs first, from the RAM disk it started with.  It mounts that volume, makes it
 * the root (sysctl kern.hakios_switch_root; the RAM disk goes to /private/preboot/boot), gives it a
 * memory filesystem for /private/var with the directories a system looks for there, and becomes the
 * volume's own /sbin/launchd -- Apple's, its code lowered (tools/lower).
 *
 *   init26_dev=NAME        the device of the volume, default rvblk0
 *   init26_exec=PATH       what to become, default /sbin/launchd
 */
#include "mini.h"

#define MNT_RDONLY_     1
#define RB_HALT         0x08

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

static void
fail(const char *what)
{
	printf("init26: %s: %s\n", what, strerror(errno));
	reboot(RB_HALT);
	_exit(1);
}

/* the directories of the data volume a system expects to find (mode, owner, path below /private/var) */
static const struct { int mode, uid; const char *path; } var_dirs[] = {
	{ 0755, 0, "db" }, { 0755, 0, "db/launchd.db" }, { 0755, 0, "db/diagnostics" }, { 0755, 0, "db/uuidtext" },
	{ 0755, 0, "db/timezone" }, { 0755, 0, "run" }, { 01777, 0, "tmp" }, { 0755, 0, "log" }, { 0755, 0, "empty" },
	{ 0755, 0, "vm" }, { 0755, 0, "root" }, { 0755, 0, "root/Library" }, { 0755, 501, "mobile" },
	{ 0755, 501, "mobile/Library" }, { 0755, 501, "mobile/Library/Caches" }, { 0755, 501, "mobile/Library/Preferences" },
	{ 0755, 501, "mobile/Library/Logs" }, { 0755, 501, "mobile/Media" }, { 0755, 0, "containers" },
	{ 0755, 0, "containers/Shared" }, { 0755, 0, "containers/Shared/SystemGroup" }, { 0755, 0, "containers/Data" },
	{ 0755, 0, "containers/Data/System" }, { 0755, 0, "preferences" }, { 0755, 0, "Managed Preferences" },
	{ 0755, 0, "MobileAsset" }, { 0755, 0, "folders" }, { 0755, 0, "keybags" }, { 0755, 0, "installd" },
	{ 0755, 0, "logs" }, { 0755, 0, "mnt" }, { 0755, 0, "networkd" }, { 0755, 0, "protected" },
	{ 0755, 0, "wireless" }, { 0755, 0, "wireless/Library" }, { 0755, 0, "hardware" }, { 0755, 0, "lib" },
	{ 0755, 0, "local" }, { 0755, 0, "msgs" }, { 0755, 0, "audit" }, { 0755, 0, "backups" }, { 0755, 0, "cache" },
	{ 0755, 0, "spool" }, { 0755, 0, "staged_system_apps" }, { 0755, 0, "personalized_factory" },
};

int
main(int argc, char **argv, char **envp)
{
	const char *dev = boot_arg("init26_dev"), *prog = boot_arg("init26_exec");
	static char devpath[160], progpath[160], path[256];
	char *new_argv[] = { progpath, NULL };
	int fd = open("/dev/console", 2 /* O_RDWR */, 0);

	(void)argc, (void)argv;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}
	strcpy(devpath, "/dev/");
	strcpy(devpath + 5, dev != NULL ? dev : "rvblk0");
	strcpy(progpath, prog != NULL ? prog : "/sbin/launchd");
	printf("\n=== init26: process %d; the system is the volume on %s ===\n", getpid(), devpath);

	if (mount("apfs", "/flash", MNT_RDONLY_, (void *)devpath) != 0) {
		fail("mount the system volume on /flash");
	}
	if (sysctlbyname("kern.hakios_switch_root", NULL, NULL, "/flash private/preboot/boot", 28) != 0) {
		fail("make it the root");
	}
	chdir("/");
	if (mount("ramfs", "/private/var", 0, NULL) != 0) {
		fail("a memory filesystem on /private/var");
	}
	for (unsigned i = 0; i < sizeof(var_dirs) / sizeof(var_dirs[0]); i++) {
		strcpy(path, "/private/var/");
		strcpy(path + 13, var_dirs[i].path);
		if (mkdir(path, var_dirs[i].mode & 0777) != 0) {
			printf("init26: mkdir %s: %s\n", path, strerror(errno));
		}
		chmod(path, var_dirs[i].mode);
		if (var_dirs[i].uid != 0) {
			chown(path, var_dirs[i].uid, var_dirs[i].uid);
		}
	}
	chmod("/dev/console", 0666);            /* daemons that are not root's write there too */
	printf("init26: the root is the system volume; becoming %s\n", progpath);
	execve(progpath, new_argv, envp);
	fail(progpath);
	return 1;
}
