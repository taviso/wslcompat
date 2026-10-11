/* XFIXME: mmap() on a synth fd fails because the synth handle is
 * O_PATH; the kernel rejects O_PATH fds for mmap with EACCES.
 *
 * Fix would interpose mmap, resolve via SYNTH_OP_IO passthru, and
 * substitute the real backing fd. Handlers whose backing isn't
 * mappable (e.g. a pipe) would still have to fail honestly with
 * ENODEV.
 *
 * Unshimmed: dlsym misses, test skips.
 *
 * Shimmed: synth fd created, mmap fails with EACCES on O_PATH. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <err.h>

#include "../synth.h"

#include "testutil.h"

static int file_fd = -1;

static int file_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e;
    (void)kind;
    return file_fd;
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

    char path[] = "/tmp/wslcompat_mmap_XXXXXX";
    int back = mkstemp(path);
    if (back < 0)
        err(EXIT_FAILURE, "mkstemp");
    unlink(path);

    const char payload[] = "mappable payload";
    if (write(back, payload, sizeof(payload)) != sizeof(payload))
        err(EXIT_FAILURE, "write");
    file_fd = back;

    int synth = synth_open(O_CLOEXEC, &file_ops, NULL);
    CHECK(synth >= 0, "synth_open -> %d", synth);
    if (synth < 0)
        return 1;

    void *p = mmap(NULL, sizeof(payload), PROT_READ, MAP_PRIVATE, synth, 0);
    CHECK(p != MAP_FAILED,
          "mmap(synth) -> %p %s",
          p, p == MAP_FAILED ? strerror(errno) : "");

    if (p != MAP_FAILED) {
        CHECK(memcmp(p, payload, sizeof(payload)) == 0,
              "  mmap content matches payload");
        munmap(p, sizeof(payload));
    }

    void *p64 = mmap64(NULL, sizeof(payload), PROT_READ, MAP_PRIVATE, synth, 0);
    CHECK(p64 != MAP_FAILED,
          "mmap64(synth) -> %p %s",
          p64, p64 == MAP_FAILED ? strerror(errno) : "");

    if (p64 != MAP_FAILED) {
        CHECK(memcmp(p64, payload, sizeof(payload)) == 0,
              "  mmap64 content matches payload");
        munmap(p64, sizeof(payload));
    }

    close(synth);
    close(back);

    TEST_REPORT();
}
