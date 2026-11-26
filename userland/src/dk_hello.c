/* A dynamically linked program for the DriverKit runtime: dyld, the (small) DriverKit shared cache, libSystem. */
/* (declared here: the DriverKit SDK's headers keep most of the C library to themselves) */
typedef unsigned long size_t;
void *malloc(size_t);
void free(void *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int atexit(void (*)(void));
size_t strlen(const char *);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);

/* (the DriverKit SDK has no open() or write(): the two system calls made directly, as userland/src/hello.c makes them) */
static long
syscall3(long number, long a, long b, long c)
{
	register long x16 __asm__("x16") = number;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;

	__asm__ volatile ("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory", "cc");
	return x0;
}
#define SYS_write 4
#define SYS_open 5

static int fd = 2;

static void say(const char *s) { syscall3(SYS_write, fd, (long)s, (long)strlen(s)); }

static int compare(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void bye(void) { say("dk_hello: atexit handler ran\n"); }

/*
 * A DriverKit program has no LC_MAIN: dyld calls what was registered with it, which is DriverKit.framework's doing in a
 * real driver. Here the program registers its own.
 */
extern void _dyld_register_driverkit_main(void (*mainFunc)(void));
static int hello_main(int argc, char **argv);
void exit(int);

static void driver_main(void) { exit(hello_main(0, (char **)0)); }

__attribute__((constructor))
static void
register_main(void)
{
	_dyld_register_driverkit_main(driver_main);
}

static int
hello_main(int argc, char **argv)
{
	int numbers[] = { 5, 3, 9, 1, 7 };
	char *p, line[64] = "dk_hello: sorted:";

	fd = (int)syscall3(SYS_open, (long)"/dev/console", 1 /* O_WRONLY */, 0);
	say("dk_hello: main() reached through dyld and libSystem\n");
	p = malloc(64);                                 /* the allocator's own set-up, in libsystem_malloc */
	strcpy(p, "dk_hello: malloc and strcpy work\n");
	say(p);
	free(p);
	qsort(numbers, 5, sizeof(int), compare);        /* libSystem calls back through a pointer of ours */
	for (int i = 0; i < 5; i++) {
		size_t n = strlen(line);

		line[n] = ' '; line[n + 1] = (char)('0' + numbers[i]); line[n + 2] = 0;
	}
	strcat(line, "\n");
	say(line);
	atexit(bye);
	(void)argc; (void)argv;
	return 0;
}
