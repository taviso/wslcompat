/* Verify dup2/dup3 boundary semantics the shim relies on:
 *
 *   dup2(fd, fd) returns fd if fd is open, -1/EBADF otherwise.
 *   dup3(fd, fd, 0) returns -1/EINVAL unconditionally.
 *   dup2(fd, n) for n >= RLIMIT_NOFILE returns -1/EBADF.
 *
 * The last one matters for our preexec_all scan in synth.c: we
 * iterate fd numbers 0..RLIMIT_NOFILE expecting all live fds to be
 * in that range. If WSL1 ever let dup2 break out of the rlimit,
 * we'd silently leak post-exec wraps. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* dup2(fd, fd) on an open fd returns fd. */
    int r = dup2(0, 0);
    CHECK(r == 0, "dup2(0, 0) -> %d (expected 0)", r);

    /* dup2(fd, fd) on a closed fd returns -1/EBADF. */
    errno = 0;
    r = dup2(999, 999);
    CHECK(r == -1 && errno == EBADF,
          "dup2(999, 999) -> %d errno=%d (expected -1/EBADF)", r, errno);

    /* dup2 on an O_PATH fd to itself: returns the fd. The shim's
     * dup_common short-circuit hands this case to native dup2. */
    int p = open("/tmp", O_PATH | O_CLOEXEC);
    if (p < 0) {
        perror("open /tmp O_PATH");
        return 1;
    }
    errno = 0;
    r = dup2(p, p);
    CHECK(r == p && errno == 0,
          "dup2(O_PATH=%d, same) -> %d errno=%d", p, r, errno);
    close(p);

    /* dup3(fd, fd, 0) returns -1/EINVAL even when fd is open. */
    errno = 0;
    r = dup3(0, 0, 0);
    CHECK(r == -1 && errno == EINVAL,
          "dup3(0, 0, 0) -> %d errno=%d (expected -1/EINVAL)", r, errno);

    /* dup2 targets at or above RLIMIT_NOFILE must fail with EBADF.
     * synth_preexec_all relies on this: it scans 0..RLIMIT_NOFILE
     * and assumes nothing dup'd higher. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        perror("getrlimit");
        return 1;
    }
    int sink = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (sink < 0) {
        perror("open /dev/null");
        return 1;
    }
    int targets[] = {
        (int)rl.rlim_cur,         /* exactly at limit */
        (int)rl.rlim_cur + 1000,  /* well past */
    };
    for (size_t i = 0; i < sizeof(targets)/sizeof(targets[0]); i++) {
        errno = 0;
        r = dup2(sink, targets[i]);
        CHECK(r == -1 && errno == EBADF,
              "dup2(sink, %d >= RLIMIT_NOFILE=%lu) -> %d errno=%d (expected -1/EBADF)",
              targets[i], (unsigned long)rl.rlim_cur, r, errno);
        if (r >= 0) close(r);
    }
    close(sink);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
