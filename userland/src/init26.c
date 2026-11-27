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
 * launchd would start all 600 jobs of the system. What it is given instead is this image's choice of them
 * (tools/ios26/boot-image.sh, userland/mkjobs.py: a few named ones, the rest only when something asks for their
 * services), put over the volume's own directories without writing to it, as userland/src/init.c does for the
 * Darwin volume on the phone's flash: /LaunchDaemons here over /System/Library/LaunchDaemons, an empty directory
 * over the other lists, and /xpc -- the volume's without its launchd.plist, the list of every job that launchd
 * would read instead of the directory -- over /System/Library/xpc.  A boot image without /LaunchDaemons: none of
 * that, and launchd starts everything.
 *
 *   init26_tasks=0         none of launchd's boot tasks run here (boot_task below)
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

/* A directory of this image shown in place of one of the volume's (bindfs); one the volume has not got: nothing. */
static void
bind_dir(const char *from, const char *to)
{
	struct stat st;

	if (stat(to, &st) != 0) {
		return;
	}
	if (mount("bindfs", to, MNT_RDONLY_, (void *)from) != 0) {
		printf("init26: bindfs %s on %s: %s\n", from, to, strerror(errno));
	}
}

/*
 * A boot task: launchd runs a list of them before any job -- its own, inside it -- and in a boot from a RAM disk
 * (rd=md0, which this is to launchd) leaves most of them out as a restore's business.  Those the system cannot do
 * without are run here, before launchd is.
 */
static void
boot_task(char *const argv[], char *const envp[])
{
	int status = 0;
	pid_t pid = fork();

	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("init26: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (pid < 0 || wait4(pid, &status, 0, NULL) != pid) {
		printf("init26: boot task %s: %s\n", argv[0], strerror(errno));
		return;
	}
	if ((status & 0x7f) != 0) {
		printf("init26: boot task %s %s: killed by signal %d\n", argv[0], argv[1] ? argv[1] : "", status & 0x7f);
	} else {
		printf("init26: boot task %s %s: exit %d\n", argv[0], argv[1] ? argv[1] : "", (status >> 8) & 0xff);
	}
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
	/* (the keychain: /Library/Keychains is a link to it; without it securityd's mkpath_np failed, EIO, and bluetoothd
	 * stopped every ten seconds on an identity it could not keep, "Bluetooth error - restarting", reason 130) */
	{ 0755, 64, "Keychains" },     /* (_securityd's, 64: securityd runs as it, "open(.../keychain-2.db) - Permission denied") */
};

/*
 * The files this image has for /private/var (its /var: tools/ios26/boot-image.sh), put there as the system starts on an
 * empty one every time: the note that the data has been migrated for this build -- without it the window server holds
 * the shell's check-in back for a migration that never ends here, and SpringBoard waits in FBSystemShellInitialize --
 * and the settings userland/var has for the 16.5 system (no setup assistant, longer watchdogs). What is below mobile/
 * is mobile's. Directories are made as they are met; a file that is there already is left.
 */
static void
var_files(const char *from, const char *to, int uid)
{
	static char buf[16384];         /* (one level's entries are used up before the next is read: path names are copied out) */
	char names[24][96];
	uint8_t types[24];
	off_t position = 0;
	ssize_t n;
	int count = 0, fd = open(from, 0 /* O_RDONLY */, 0);

	if (fd < 0) {
		return;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);

			at += de->d_reclen;
			if (de->d_reclen == 0) {
				break;
			}
			if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(names[0]) || count == 24) {
				continue;
			}
			strcpy(names[count], de->d_name);
			types[count++] = de->d_type;
		}
	}
	close(fd);
	for (int i = 0; i < count; i++) {
		char src[320], dst[320];
		int owner = uid;

		if (strlen(from) + strlen(names[i]) + 2 > sizeof(src) || strlen(to) + strlen(names[i]) + 2 > sizeof(dst)) {
			continue;
		}
		strcpy(src, from); strcpy(src + strlen(src), "/"); strcpy(src + strlen(src), names[i]);
		strcpy(dst, to); strcpy(dst + strlen(dst), "/"); strcpy(dst + strlen(dst), names[i]);
		if (strcmp(names[i], "mobile") == 0) {
			owner = 501;
		}
		if (types[i] == 4 /* DT_DIR */) {
			if (mkdir(dst, 0755) == 0) {
				chown(dst, owner, owner);
			}
			var_files(src, dst, owner);
		} else if (types[i] == 8 /* DT_REG */) {
			int in = open(src, 0, 0), out = open(dst, 0x0001 | 0x0200 | 0x0800 /* O_WRONLY | O_CREAT | O_EXCL */, 0644);
			static char data[65536];
			ssize_t got;

			while (in >= 0 && out >= 0 && (got = read(in, data, sizeof(data))) > 0) {
				if (write(out, data, (size_t)got) != got) {
					printf("init26: %s: %s\n", dst, strerror(errno));
					break;
				}
			}
			if (out >= 0) {
				close(out);
				chown(dst, owner, owner);
				printf("init26: %s\n", dst);
			}
			if (in >= 0) {
				close(in);
			}
		}
	}
}

int
main(int argc, char **argv, char **envp)
{
	const char *dev = boot_arg("init26_dev"), *prog = boot_arg("init26_exec");
	static char devpath[160], progpath[160], path[256];
	char *new_argv[] = { progpath, NULL };
	struct stat jobs;
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
	if (stat("/LaunchDaemons", &jobs) == 0) {
		bind_dir("/LaunchDaemons", "/flash/System/Library/LaunchDaemons");
		bind_dir("/empty", "/flash/System/Library/NanoLaunchDaemons");
		bind_dir("/empty", "/flash/System/Library/NanoLaunchDaemonsAltAccount");
		bind_dir("/empty", "/flash/System/Library/DeveloperModeLaunchDaemons");
		bind_dir("/empty", "/flash/System/Library/LaunchAngels");
		bind_dir("/xpc", "/flash/System/Library/xpc");
		printf("init26: this image's launchd jobs, not the volume's\n");
	}
	{
		/*
		 * The applications the system installs on its first start -- Camera, Photos, Safari, Messages, the clock, the
		 * wallpapers' own: 49 of them, in /private/var/staged_system_apps of the volume -- among the system's own in
		 * /Applications: another name for each there (kern.apfs_synthetic: "NEW=EXISTING", an entry the volume has not
		 * got, as userland/src/init.c makes them). /private/var is a memory filesystem here and covers where they lie,
		 * nothing installs them, and the home screen had Settings and Phone and nothing else.
		 */
		static char buf[16384], name[128], both[400];
		off_t position = 0;
		ssize_t n;
		int apps = 0, named = 0, dir = open("/flash/private/var/staged_system_apps", 0, 0);

		while (dir >= 0 && (n = getdirentries(dir, buf, sizeof(buf), &position)) > 0) {
			for (ssize_t at = 0; at < n;) {
				struct dirent *de = (struct dirent *)(buf + at);
				size_t len = de->d_namlen;

				if (de->d_reclen == 0) {
					break;
				}
				at += de->d_reclen;
				if (len < 5 || len >= sizeof(name) || strncmp(de->d_name + len - 4, ".app", 4) != 0) {
					continue;
				}
				memcpy(name, de->d_name, len);
				name[len] = 0;
				apps++;
				strcpy(both, "/flash/Applications/");
				strcpy(both + strlen(both), name);
				strcpy(both + strlen(both), "=/flash/private/var/staged_system_apps/");
				strcpy(both + strlen(both), name);
				if (sysctlbyname("kern.apfs_synthetic", NULL, NULL, both, strlen(both)) == 0) {
					named++;
				} else if (named == 0 && apps == 1) {
					printf("init26: %s: %s\n", both, strerror(errno));
				}
			}
		}
		if (dir >= 0) {
			close(dir);
			printf("init26: %d of the %d applications installed at a first start are in /Applications\n", named, apps);
		}
		/*
		 * ... and the directory itself under a second name outside /private/var, for the memory filesystem that is about
		 * to cover it: LaunchServices takes an application of /Applications back to where its files really are, and
		 * found nothing at /private/var/staged_system_apps ("... with no CFBundleIdentifier in their plist", 143 times).
		 * See below, where it is put back in its place.
		 */
		strcpy(both, "/flash/System/Library/staged_system_apps=/flash/private/var/staged_system_apps");
		if (dir >= 0 && sysctlbyname("kern.apfs_synthetic", NULL, NULL, both, strlen(both)) != 0) {
			printf("init26: %s: %s\n", both, strerror(errno));
		}
	}
	{
		/*
		 * /fw of the root, where the kernel's drivers look for the phone's firmware once there is a root (the modem's
		 * files that it reads over TFTP, the audio DSP's image, Bluetooth's patch): this image's fw/, under a name the
		 * volume has not got (kern.apfs_synthetic: a directory made of nothing).
		 */
		struct stat st;

		if (stat("/fw", &st) == 0 && stat("/flash/fw", &st) != 0) {
			if (sysctlbyname("kern.apfs_synthetic", NULL, NULL, "/flash/fw", 9) != 0) {
				printf("init26: /flash/fw: %s\n", strerror(errno));
			}
			bind_dir("/fw", "/flash/fw");
		}
		/*
		 * ... and the audio server's plug-in of this image's (userland/src/audio_plugin.c: the speaker, the microphone,
		 * the motor as an iPhone's devices), among the volume's in /System/Library/Audio/Plug-Ins/HAL, the same way.
		 */
		if (stat("/hal/HakiosAudio.driver", &st) == 0) {
			static const char hal[] = "/flash/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver";

			if (stat(hal, &st) != 0 && sysctlbyname("kern.apfs_synthetic", NULL, NULL, hal, sizeof(hal) - 1) != 0) {
				printf("init26: %s: %s\n", hal, strerror(errno));
			}
			bind_dir("/hal/HakiosAudio.driver", hal);
		}
	}
	{
		/*
		 * The programs that are started with a library of this image's in them (kern.exec_inject: "LIBRARY NAME ...",
		 * groups parted by ";" -- bsd/kern/kern_exec.c), as this image's /etc/inject.conf has them
		 * (tools/ios26/boot-image.sh): the window server with the one that gives it the phone's touch screen.
		 */
		static char conf[512];
		int in = open("/etc/inject.conf", 0, 0);
		ssize_t n = in >= 0 ? read(in, conf, sizeof(conf) - 1) : 0;

		if (in >= 0) {
			close(in);
		}
		while (n > 0 && (conf[n - 1] == '\n' || conf[n - 1] == ' ')) {
			n--;
		}
		if (n > 0) {
			conf[n] = 0;
			if (sysctlbyname("kern.exec_inject", NULL, NULL, conf, (size_t)n + 1) != 0) {
				printf("init26: kern.exec_inject: %s\n", strerror(errno));
			} else {
				printf("init26: libraries of this image's in: %s\n", conf);
			}
		}
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
	if (mount("bindfs", "/private/var/staged_system_apps", MNT_RDONLY_, (void *)"/System/Library/staged_system_apps") != 0) {
		printf("init26: the applications of a first start on /private/var/staged_system_apps: %s\n", strerror(errno));
	}
	var_files("/private/preboot/boot/var", "/private/var", 0);
	{
		/* (launchd's own: "usermanagerd" -- the user and its personas, /private/var/keybags; without them the daemon stops) */
		char *usermanagerd[] = { "/usr/libexec/usermanagerd", "--init", NULL };
		const char *tasks = boot_arg("init26_tasks");

		if (tasks == NULL || strcmp(tasks, "0") != 0) {
			boot_task(usermanagerd, envp);
		}
	}
	printf("init26: the root is the system volume; becoming %s\n", progpath);
	execve(progpath, new_argv, envp);
	fail(progpath);
	return 1;
}
