/* XFIXME: splice() with a synth fd as source fails because the synth
 * handle is O_PATH; the kernel needs a read-capable fd. Returns EBADF.
 *
 * Fix would be similar to sendfile: interpose splice, resolve via
 * SYNTH_OP_IO passthru, forward to the real backing.
 *
 * Unshimmed: dlsym misses, test skips.
 *
 * Shimmed: synth fd created, splice fails with EBADF on O_PATH. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int src_read_fd = -1;

static int src_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e;
    (void)kind;
    return src_read_fd;
}

static const struct synth_ops src_ops = {
    .passthru = src_passthru,
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

    int src[2], dst[2];
    if (pipe(src) < 0) err(EXIT_FAILURE, "pipe src");
    if (pipe(dst) < 0) err(EXIT_FAILURE, "pipe dst");
    src_read_fd = src[0];

    int synth = synth_open(O_CLOEXEC, &src_ops, NULL);
    CHECK(synth >= 0, "synth_open -> %d", synth);
    if (synth < 0)
        return 1;

    const char payload[] = "splice me";
    if (write(src[1], payload, sizeof(payload)) != sizeof(payload))
        err(EXIT_FAILURE, "src write");

    ssize_t n = splice(synth, NULL, dst[1], NULL, sizeof(payload), 0);
    CHECK(n == (ssize_t)sizeof(payload),
          "splice(synth -> dst) -> %zd (expected %zu) %s",
          n, sizeof(payload), n < 0 ? strerror(errno) : "");

    if (n == (ssize_t)sizeof(payload)) {
        char buf[64];
        ssize_t r = read(dst[0], buf, sizeof(buf));
        CHECK(r == n && memcmp(buf, payload, n) == 0,
              "  dst received %zd bytes matching payload", r);
    }

    close(synth);
    close(src[0]); close(src[1]);
    close(dst[0]); close(dst[1]);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
