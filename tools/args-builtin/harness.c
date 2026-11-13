/* tools/args-builtin/harness.c: the host side of tools/args-builtin-test.py. It is linked with the REAL pexpert/gen/bootargs.c
 * (and the header that holds the built-in words) and answers, for each name on stdin, what the kernel's PE_parse_boot_argn and
 * PE_parse_boot_arg_str would say with the boot line given as argv[1] (nothing if argv[1] is "-"). One line out per name:
 *   name <TAB> found-as-number <TAB> number <TAB> found-as-string <TAB> string <TAB> found-as-presence
 * A name is looked for as the kernel's callers do: a 4-byte number, a 128-byte string, and with max_len 0 (presence). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pexpert/pexpert.h>

static char line[1024];

char *PE_boot_args(void) { return line; }
int SecureDTLookupEntry(void *a, const char *b, DTEntry *c) { (void)a; (void)b; (void)c; return -1; }
int SecureDTGetProperty(DTEntry a, const char *b, void const **c, unsigned int *d) { (void)a; (void)b; (void)c; (void)d; return -1; }
int IODTGetDefault(const char *key, void *infoAddr, unsigned int infoSize) { (void)key; (void)infoAddr; (void)infoSize; return -1; }

int main(int argc, char **argv)
{
	char name[256];
	if (argc != 2) {
		fprintf(stderr, "usage: harness BOOTLINE < names\n");
		return 2;
	}
	if (strcmp(argv[1], "-") != 0) {
		if (strlen(argv[1]) > 607) {
			fprintf(stderr, "harness: the boot line holds 607 characters, this one has %zu\n", strlen(argv[1]));
			return 2;
		}
		strcpy(line, argv[1]);
	}
	while (fgets(name, sizeof name, stdin)) {
		name[strcspn(name, "\n")] = 0;
		int32_t num = (int32_t)0xCAFEFACE;
		char str[128] = "NOT_FOUND";
		boolean_t fn = PE_parse_boot_argn(name, &num, sizeof num);
		boolean_t fs = PE_parse_boot_arg_str(name, str, sizeof str);
		boolean_t fp = PE_parse_boot_argn(name, NULL, 0);
		printf("%s\t%d\t%d\t%d\t%s\t%d\n", name, fn, fn ? num : 0, fs, fs ? str : "", fp);
	}
	return 0;
}
