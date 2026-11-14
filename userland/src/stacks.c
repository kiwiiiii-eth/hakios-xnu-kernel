/*
 * stacks: where the threads of a process are, and what each waits for -- the phone has no debugger, no sample, no
 * spindump. Safari's main thread stopped handling WebKit's messages a moment after a page's load had started (r157: the
 * page loaded and was drawn in WebKit's processes, and the application never heard of it); this says what it waits in.
 *
 *   usage: stacks PID|all [MOST]  every thread of PID (or of every process): its name, the kernel's state, what it waits for (a port and the
 *                                 process that holds it, a lock and the thread that holds it...), and up to MOST frames
 *                                 of its user stack (default 40), as "address  library`symbol + offset"
 *
 * Through the kernel's stackshot (what spindump and the hang reports are made of), not task ports: a platform binary's
 * control port is not given to a program that is not one (task_conversion_eval: r158, "invalid destination port"), and
 * its read port does not give registers ("invalid argument"). A stackshot only wants root (bsd/kern/stackshot.c;
 * src/stacks.entitlements has com.apple.private.stackshot as well). Its kcdata (osfmk/kern/kcdata.h) is walked here:
 * the task and thread containers, each thread's snapshot, name and user return addresses, and at the end the wait
 * information of every waiting thread.
 *
 * Names come from dladdr in this process: the shared cache, where every system library is, has the same slide in every
 * process of a boot, so its addresses name the same functions here; an address in the application's own executable is
 * printed bare.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libsystem_kernel's, which the SDK does not declare (libsyscall/wrappers/stackshot.c) */
void *stackshot_config_create(void);
int stackshot_config_set_pid(void *config, int pid);
int stackshot_config_set_flags(void *config, uint64_t flags);
int stackshot_capture_with_config(void *config);
void *stackshot_config_get_stackshot_buffer(void *config);
uint32_t stackshot_config_get_stackshot_size(void *config);
int stackshot_config_dealloc(void *config);

#define STACKSHOT_GET_DQ                0x01
#define STACKSHOT_KCDATA_FORMAT         0x10000
#define STACKSHOT_THREAD_WAITINFO       0x1000000

#define KCDATA_TYPE_CONTAINER_BEGIN     0x13u
#define KCDATA_TYPE_CONTAINER_END       0x14u
#define KCDATA_TYPE_ARRAY_PAD0          0x20u
#define KCDATA_TYPE_BUFFER_END          0xF19158EDu
#define STACKSHOT_KCCONTAINER_THREAD    0x904u
#define STACKSHOT_KCTYPE_TASK_SNAPSHOT  0x905u
#define STACKSHOT_KCTYPE_THREAD_SNAPSHOT 0x906u
#define STACKSHOT_KCTYPE_THREAD_NAME    0x909u
#define STACKSHOT_KCTYPE_USER_STACKFRAME64 0x90Du
#define STACKSHOT_KCTYPE_USER_STACKLR64 0x916u
#define STACKSHOT_KCTYPE_THREAD_WAITINFO 0x91du

struct item {
	uint32_t type, size;
	uint64_t flags;
};

static const char *wait_kinds[] = {
	"nothing", "a kernel mutex", "a port's message (receive)", "a port set's message", "room in a port (send)",
	"a port in transit", "a semaphore", "a kernel rwlock (read)", "a kernel rwlock (write)", "a kernel rwlock (upgrade)",
	"a user lock", "a pthread mutex", "a pthread rwlock (read)", "a pthread rwlock (write)", "a condition variable",
	"work (parked workqueue thread)", "a workloop's sync wait", "a process", "sleep with inheritor", "an eventlink",
	"the compressor",
};

static void
name(uint64_t address, char *out, size_t size)
{
	Dl_info info;

	if (dladdr((const void *)(uintptr_t)address, &info) != 0 && info.dli_fname != NULL) {
		const char *library = strrchr(info.dli_fname, '/');

		library = library != NULL ? library + 1 : info.dli_fname;
		if (info.dli_sname != NULL) {
			snprintf(out, size, "%s`%s + %llu", library, info.dli_sname,
			    (unsigned long long)(address - (uint64_t)(uintptr_t)info.dli_saddr));
		} else {
			snprintf(out, size, "%s + 0x%llx", library, (unsigned long long)(address - (uint64_t)(uintptr_t)info.dli_fbase));
		}
	} else {
		snprintf(out, size, "?");
	}
}

int
main(int argc, char **argv)
{
	int pid = argc > 1 ? atoi(argv[1]) : 0, most = argc > 2 ? atoi(argv[2]) : 40, error;
	void *config;
	const uint8_t *buffer, *at, *end;
	uint32_t size;
	int in_thread = 0, threads = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (pid == 0 && (argc < 2 || strcmp(argv[1], "all") != 0)) {
		fprintf(stderr, "usage: stacks PID|all [MOST]\n");
		return 1;
	}
	config = stackshot_config_create();
	if (config == NULL) {
		printf("stacks: no stackshot configuration\n");
		return 1;
	}
	if (pid > 0) {
		stackshot_config_set_pid(config, pid);          /* (else every process: "all") */
	}
	stackshot_config_set_flags(config, STACKSHOT_KCDATA_FORMAT | STACKSHOT_THREAD_WAITINFO | STACKSHOT_GET_DQ);
	if ((error = stackshot_capture_with_config(config)) != 0) {
		printf("stacks: the stackshot failed: %s (%d)\n", strerror(error), error);
		return 1;
	}
	buffer = stackshot_config_get_stackshot_buffer(config);
	size = stackshot_config_get_stackshot_size(config);
	if (buffer == NULL || size < sizeof(struct item)) {
		printf("stacks: an empty stackshot\n");
		return 1;
	}
	printf("stacks: a stackshot of %u bytes\n", size);
	at = buffer;
	end = buffer + size;
	while (at + sizeof(struct item) <= end) {
		struct item item;
		const uint8_t *data = at + sizeof(struct item);

		memcpy(&item, at, sizeof(item));
		if (item.type == KCDATA_TYPE_BUFFER_END || data + item.size > end) {
			break;
		}
		if (item.type == KCDATA_TYPE_CONTAINER_BEGIN && item.size >= 4) {
			uint32_t kind;

			memcpy(&kind, data, 4);
			in_thread = kind == STACKSHOT_KCCONTAINER_THREAD;
			threads += in_thread;
		} else if (item.type == KCDATA_TYPE_CONTAINER_END) {
			in_thread = 0;
		} else if (item.type == STACKSHOT_KCTYPE_TASK_SNAPSHOT && item.size >= 8 * 7 + 4 * 6 + 4 + 32) {
			int32_t task_pid;
			char comm[33] = "";

			memcpy(&task_pid, data + 8 * 7 + 4 * 6, 4);
			memcpy(comm, data + 8 * 7 + 4 * 6 + 4, 32);
			printf("process %d (%s)\n", task_pid, comm);
		} else if (item.type == STACKSHOT_KCTYPE_THREAD_SNAPSHOT && item.size >= 8 * 11 + 4) {
			uint64_t id, event, user_time;
			uint32_t state;

			memcpy(&id, data, 8);
			memcpy(&event, data + 8, 8);
			memcpy(&user_time, data + 48, 8);
			memcpy(&state, data + 8 * 11, 4);
			printf("\nthread 0x%llx: state 0x%x%s%s, %.3f s user, waits on event 0x%llx\n", (unsigned long long)id, state,
			    state & 0x1 ? " (waiting)" : "", state & 0x4 ? " (runnable)" : "",
			    user_time / 1e9, (unsigned long long)event);
		} else if (item.type == STACKSHOT_KCTYPE_THREAD_NAME && in_thread) {
			printf("  name: %.*s\n", (int)item.size, (const char *)data);
		} else if ((item.type & ~0xfu) == KCDATA_TYPE_ARRAY_PAD0) {
			uint32_t kind = (uint32_t)(item.flags >> 32), count = (uint32_t)item.flags;
			uint32_t bytes = item.size - (item.type & 0xf), each = count != 0 ? bytes / count : 0;

			if (kind == STACKSHOT_KCTYPE_USER_STACKLR64 || kind == STACKSHOT_KCTYPE_USER_STACKFRAME64) {
				for (uint32_t i = 0; i < count && (int)i < most && each >= 8; i++) {
					uint64_t lr;
					char where[256];

					memcpy(&lr, data + i * each, 8);
					name(lr, where, sizeof(where));
					printf("  %2u  0x%llx  %s\n", i, (unsigned long long)lr, where);
				}
			} else if (kind == STACKSHOT_KCTYPE_THREAD_WAITINFO && each >= 25) {
				printf("\nwhat the waiting threads wait for:\n");
				for (uint32_t i = 0; i < count; i++) {
					uint64_t owner, waiter, context;
					uint8_t type;

					memcpy(&owner, data + i * each, 8);
					memcpy(&waiter, data + i * each + 8, 8);
					memcpy(&context, data + i * each + 16, 8);
					type = data[i * each + 24];
					printf("  thread 0x%llx waits for %s (0x%llx), held by %lld%s\n", (unsigned long long)waiter,
					    type < sizeof(wait_kinds) / sizeof(wait_kinds[0]) ? wait_kinds[type] : "?",
					    (unsigned long long)context, (long long)owner,
					    type >= 2 && type <= 5 ? " (a process)" : type == 1 || type >= 0xa ? " (a thread)" : "");
				}
			}
		}
		at = data + item.size;
	}
	printf("\nstacks: %d threads\n", threads);
	stackshot_config_dealloc(config);
	return 0;
}
