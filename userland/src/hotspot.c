/*
 * hotspot: the phone's hotspot on and off (the kernel's sysctl kern.hakios.hotspot*, drivers/wifi/hakios.sta.c).
 *
 *   hotspot on [FILE]   the name and the passphrase from FILE (two lines; /private/var/root/hakios-hotspot by default,
 *                       which build.sh puts there from ssh/hotspot, never in the repository), then on
 *   hotspot off         off: the radio a station again, and the network it knows joined
 *   hotspot status      on or off, and the name
 *
 * The passphrase goes from the file to the kernel and nowhere else: not in argv, not said.
 */
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static int
set_string(const char *name, const char *value)
{
	return sysctlbyname(name, NULL, NULL, (void *)value, strlen(value) + 1);
}

int
main(int argc, char **argv)
{
	int on;
	size_t size = sizeof(on);

	if (argc >= 2 && strcmp(argv[1], "on") == 0) {
		const char *path = argc >= 3 ? argv[2] : "/private/var/root/hakios-hotspot";
		char name[64] = "", passphrase[80] = "";
		FILE *f = fopen(path, "r");

		if (f == NULL || fgets(name, sizeof(name), f) == NULL || fgets(passphrase, sizeof(passphrase), f) == NULL) {
			fprintf(stderr, "hotspot: %s: no name and passphrase (two lines)\n", path);
			if (f != NULL) {
				fclose(f);
			}
			return 1;
		}
		fclose(f);
		name[strcspn(name, "\r\n")] = 0;
		passphrase[strcspn(passphrase, "\r\n")] = 0;
		if (set_string("kern.hakios.hotspot_name", name) != 0 || set_string("kern.hakios.hotspot_password", passphrase) != 0) {
			memset(passphrase, 0, sizeof(passphrase));
			perror("hotspot: the name or the passphrase (8 to 63 characters)");
			return 1;
		}
		memset(passphrase, 0, sizeof(passphrase));
		on = 1;
		if (sysctlbyname("kern.hakios.hotspot", NULL, NULL, &on, sizeof(on)) != 0) {
			perror("hotspot: on");
			return 1;
		}
		printf("hotspot: asked on: \"%s\"\n", name);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "off") == 0) {
		on = 0;
		if (sysctlbyname("kern.hakios.hotspot", NULL, NULL, &on, sizeof(on)) != 0) {
			perror("hotspot: off");
			return 1;
		}
		printf("hotspot: asked off\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		char name[64] = "";
		size_t length = sizeof(name);

		if (sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) != 0) {
			perror("hotspot: status");
			return 1;
		}
		(void)sysctlbyname("kern.hakios.hotspot_name", name, &length, NULL, 0);
		printf("hotspot: %s (\"%s\")\n", on ? "ON" : "off", name);
		return 0;
	}
	fprintf(stderr, "usage: hotspot on [FILE] | off | status\n");
	return 2;
}
