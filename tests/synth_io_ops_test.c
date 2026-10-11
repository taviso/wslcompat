/* Exercises the SYNTH_SHIM / SYNTH_SHIM_E dispatch path for the
 * one-line per-fd ops that just forward to the handler's SYNTH_OP_IO
 * fd via sym_next. None of these have bespoke logic in the shim --
 * they ride entirely on the synth.h dispatch machinery.
 *
 * Unshimmed: dlsym misses wslcompat_synth_open, skip. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/xattr.h>
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

    char path[] = "/tmp/wslcompat_io_XXXXXX";
    back_fd = mkstemp(path);
    if (back_fd < 0)
        err(EXIT_FAILURE, "mkstemp");
    unlink(path);

    const char seed[] = "0123456789abcdef";
    if (write(back_fd, seed, sizeof(seed) - 1) != (ssize_t)(sizeof(seed) - 1))
        err(EXIT_FAILURE, "seed write");

    int synth = synth_open(O_CLOEXEC, &file_ops, NULL);
    if (synth < 0)
        err(EXIT_FAILURE, "synth_open");

    char buf[16];

    ssize_t pr = pread(synth, buf, 4, 0);
    CHECK(pr == 4, "pread(synth, 4, 0) -> %zd %s",
          pr, pr < 0 ? strerror(errno) : "");

    ssize_t pw = pwrite(synth, "ZZZZ", 4, 0);
    CHECK(pw == 4, "pwrite(synth, 4, 0) -> %zd %s",
          pw, pw < 0 ? strerror(errno) : "");

    // Current pos is at 16 after the seed write; readv at EOF
    // legitimately returns 0, so check for "did not error" instead
    // of "read something".
    struct iovec iv = { .iov_base = buf, .iov_len = sizeof(buf) };
    ssize_t rv = readv(synth, &iv, 1);
    CHECK(rv != -1, "readv(synth) -> %zd %s",
          rv, rv < 0 ? strerror(errno) : "");

    struct iovec wv = { .iov_base = (void *)"WVWV", .iov_len = 4 };
    ssize_t wvn = writev(synth, &wv, 1);
    CHECK(wvn == 4, "writev(synth) -> %zd %s",
          wvn, wvn < 0 ? strerror(errno) : "");

    ssize_t prv = preadv(synth, &iv, 1, 0);
    CHECK(prv > 0, "preadv(synth) -> %zd %s",
          prv, prv < 0 ? strerror(errno) : "");

    ssize_t pwv = pwritev(synth, &wv, 1, 0);
    CHECK(pwv == 4, "pwritev(synth) -> %zd %s",
          pwv, pwv < 0 ? strerror(errno) : "");

    int fs = fsync(synth);
    CHECK(fs == 0, "fsync(synth) -> %d %s",
          fs, fs < 0 ? strerror(errno) : "");

    int fds = fdatasync(synth);
    CHECK(fds == 0, "fdatasync(synth) -> %d %s",
          fds, fds < 0 ? strerror(errno) : "");

    int ft = ftruncate(synth, 8);
    CHECK(ft == 0, "ftruncate(synth, 8) -> %d %s",
          ft, ft < 0 ? strerror(errno) : "");

    int fcm = fchmod(synth, 0600);
    CHECK(fcm == 0, "fchmod(synth, 0600) -> %d %s",
          fcm, fcm < 0 ? strerror(errno) : "");

    int fco = fchown(synth, getuid(), getgid());
    CHECK(fco == 0, "fchown(synth) -> %d %s",
          fco, fco < 0 ? strerror(errno) : "");

    int fal = fallocate(synth, 0, 0, 4096);
    CHECK(fal == 0, "fallocate(synth, 0, 0, 4096) -> %d %s",
          fal, fal < 0 ? strerror(errno) : "");

    int sx = fsetxattr(synth, "user.wslcompat.test", "v", 1, 0);
    CHECK(sx == 0, "fsetxattr(synth) -> %d %s",
          sx, sx < 0 ? strerror(errno) : "");

    char xbuf[64];
    ssize_t gx = fgetxattr(synth, "user.wslcompat.test", xbuf, sizeof(xbuf));
    CHECK(gx == 1 && xbuf[0] == 'v', "fgetxattr(synth) -> %zd %s",
          gx, gx < 0 ? strerror(errno) : "");

    ssize_t lx = flistxattr(synth, xbuf, sizeof(xbuf));
    CHECK(lx > 0, "flistxattr(synth) -> %zd %s",
          lx, lx < 0 ? strerror(errno) : "");

    close(synth);
    close(back_fd);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
