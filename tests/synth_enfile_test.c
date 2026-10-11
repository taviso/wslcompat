/* Verify ENFILE propagates from wslcompat_synth_open when the synth
 * table is exhausted. The mmap_min_addr handler must "claim" the path
 * (return 1 from dispatch) even on synth failure; otherwise open.c
 * would fall through to the kernel and clobber errno (ENOENT on WSL1,
 * success on real Linux).
 *
 * Probes via dlsym so the unshimmed run skips cleanly -- the kernel's
 * real /proc/sys/vm/mmap_min_addr has no SYNTH_MAX equivalent. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_FDS 256

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

int main(void)
{
    if (dlsym(RTLD_DEFAULT, "wslcompat_synth_open") == NULL) {
        printf("  SKIP: libwslcompat not loaded\n");
        return 0;
    }

    int fds[MAX_FDS];
    int n_open = MAX_FDS;

    for (int i = 0; i < MAX_FDS; i++) {
        fds[i] = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
        if (fds[i] < 0) {
            n_open = i;
            CHECK(errno == ENFILE,
                  "got ENFILE on exhaustion (errno=%d: %s) after %d opens",
                  errno, strerror(errno), i);
            break;
        }
    }

    CHECK(n_open < MAX_FDS,
          "hit a cap before %d opens (n_open=%d)", MAX_FDS, n_open);

    for (int i = 0; i < n_open; i++)
        close(fds[i]);

    /* Slots should release on close; one more open must succeed. */
    int probe = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
    CHECK(probe >= 0,
          "fresh open succeeds after closes: %s",
          probe < 0 ? strerror(errno) : "ok");
    if (probe >= 0) close(probe);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
