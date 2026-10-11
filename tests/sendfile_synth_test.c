/* XFIXME: sendfile(out, in, ...) where `in` is a synth fd fails because
 * the synth handle is O_PATH and the kernel needs a read()-capable fd
 * to copy from. Returns EBADF.
 *
 * To support this we'd need to interpose sendfile, resolve the synth
 * via SYNTH_OP_IO passthru, and forward to sym_next(sendfile, real, ...).
 * For backings that can't satisfy sendfile semantics (pipe in_fd is
 * rejected by older kernels), we'd have to fall back to a read/write
 * bounce buffer.
 *
 * Unshimmed: dlsym misses, test skips.
 *
 * Shimmed: synth fd created, sendfile fails with EBADF on O_PATH. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
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

    char path[] = "/tmp/wslcompat_sendfile_XXXXXX";
    int back = mkstemp(path);
    if (back < 0)
        err(EXIT_FAILURE, "mkstemp");
    unlink(path);

    const char payload[] = "hello sendfile";
    if (write(back, payload, sizeof(payload)) != sizeof(payload))
        err(EXIT_FAILURE, "write");
    if (lseek(back, 0, SEEK_SET) != 0)
        err(EXIT_FAILURE, "lseek");
    file_fd = back;

    int synth = synth_open(O_CLOEXEC, &file_ops, NULL);
    CHECK(synth >= 0, "synth_open -> %d", synth);
    if (synth < 0)
        return 1;

    int sink[2];
    if (pipe(sink) < 0)
        err(EXIT_FAILURE, "pipe");

    off_t off = 0;
    ssize_t n = sendfile(sink[1], synth, &off, sizeof(payload));
    CHECK(n == (ssize_t)sizeof(payload),
          "sendfile(sink, synth) -> %zd (expected %zu) %s",
          n, sizeof(payload), n < 0 ? strerror(errno) : "");

    if (n == (ssize_t)sizeof(payload)) {
        char buf[64];
        ssize_t r = read(sink[0], buf, sizeof(buf));
        CHECK(r == n && memcmp(buf, payload, n) == 0,
              "  pipe received %zd bytes matching payload", r);
    }

    close(synth);
    close(back);
    close(sink[0]);
    close(sink[1]);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
