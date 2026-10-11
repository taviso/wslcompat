/* XPASS: posix_fadvise() is purely advisory; the kernel accepts it on
 * O_PATH fds without complaint, so it works on a synth fd today even
 * without a dedicated shim. This test pins that behaviour so a future
 * refactor doesn't quietly regress it.
 *
 * Unshimmed: dlsym misses, test skips.
 *
 * Shimmed: synth fd created, posix_fadvise returns 0. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <err.h>

#include "../synth.h"

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static int back_fd = -1;

static int file_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e;
    (void)kind;
    return back_fd;
}

static const struct synth_ops file_ops = {
    .passthru = file_passthru,
};

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int (*synth_open)(int, const struct synth_ops *, void *) =
        dlsym(RTLD_DEFAULT, "wslcompat_synth_open");
    if (synth_open == NULL) {
        printf("  SKIP: libwslcompat not loaded (no wslcompat_synth_open)\n");
        return 0;
    }

    char path[] = "/tmp/wslcompat_fadvise_XXXXXX";
    back_fd = mkstemp(path);
    if (back_fd < 0)
        err(EXIT_FAILURE, "mkstemp");
    unlink(path);

    const char seed[] = "fadvise payload";
    if (write(back_fd, seed, sizeof(seed)) != (ssize_t)sizeof(seed))
        err(EXIT_FAILURE, "seed write");

    int synth = synth_open(O_CLOEXEC, &file_ops, NULL);
    CHECK(synth >= 0, "synth_open -> %d", synth);
    if (synth < 0)
        return 1;

    int fa = posix_fadvise(synth, 0, 0, POSIX_FADV_NORMAL);
    CHECK(fa == 0, "posix_fadvise(synth, NORMAL) -> %d %s",
          fa, fa != 0 ? strerror(fa) : "");

    int fa2 = posix_fadvise(synth, 0, sizeof(seed), POSIX_FADV_SEQUENTIAL);
    CHECK(fa2 == 0, "posix_fadvise(synth, SEQUENTIAL, 0, %zu) -> %d %s",
          sizeof(seed), fa2, fa2 != 0 ? strerror(fa2) : "");

    close(synth);
    close(back_fd);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
