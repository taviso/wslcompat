/* Verify creation-time is reported and stays stable across utimes().
 *
 * The trick: utimes() only changes atime+mtime (and ctime as a side
 * effect of any inode modification), but never btime. So a "real"
 * btime source -- whether the kernel's native one on real Linux, or
 * our shim's xattr on WSL1 -- must return the same btime before and
 * after a utimes() call.
 *
 * What fails the test:
 *   - statx returns no STATX_BTIME bit at all (unpatched WSL1: kernel
 *     doesn't support it).
 *   - btime shifts when utimes() shifts mtime (i.e., the source is a
 *     min(mtime, ctime) heuristic rather than a real persistent record).
 *
 * What passes:
 *   - libwslcompat loaded: xattr backs btime, utimes doesn't touch xattr.
 *   - Native Linux: kernel-tracked btime is utimes-invariant. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static char tmppath[64];
static void cleanup(void) { if (tmppath[0]) unlink(tmppath); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    snprintf(tmppath, sizeof(tmppath), "/tmp/wslcompat_btime_%d", (int)getpid());
    unlink(tmppath);
    atexit(cleanup);

    /* Use open() directly so the LD_PRELOAD interpose actually fires.
     * mkstemp() goes through glibc's internal __open. */
    int fd = open(tmppath, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        printf("FAIL: open: %s\n", strerror(errno));
        return 1;
    }
    time_t created_at = time(NULL);

    struct statx stx_before = {0};
    if (statx(AT_FDCWD, tmppath, 0, STATX_BTIME, &stx_before) != 0) {
        printf("FAIL: statx#1: %s\n", strerror(errno));
        return 1;
    }
    CHECK(stx_before.stx_mask & STATX_BTIME,
          "statx returned STATX_BTIME (mask=0x%x)", stx_before.stx_mask);
    if (!(stx_before.stx_mask & STATX_BTIME))
        return 1;

    CHECK(stx_before.stx_btime.tv_sec >= created_at - 2 &&
          stx_before.stx_btime.tv_sec <= created_at + 2,
          "initial btime=%lld within [%lld, %lld]",
          (long long)stx_before.stx_btime.tv_sec,
          (long long)created_at - 2, (long long)created_at + 2);

    /* Move mtime/atime to year 2001. If btime is actually derived from
     * mtime/ctime (the wrong implementation), this will be visible. */
    struct timeval shifted[2] = {
        { 1000000000, 0 },   /* atime */
        { 1000000000, 0 },   /* mtime */
    };
    if (utimes(tmppath, shifted) != 0) {
        printf("FAIL: utimes: %s\n", strerror(errno));
        return 1;
    }

    struct statx stx_after = {0};
    if (statx(AT_FDCWD, tmppath, 0, STATX_BTIME, &stx_after) != 0) {
        printf("FAIL: statx#2: %s\n", strerror(errno));
        return 1;
    }
    CHECK(stx_after.stx_mask & STATX_BTIME,
          "statx#2 still returns STATX_BTIME");
    CHECK(stx_after.stx_btime.tv_sec == stx_before.stx_btime.tv_sec &&
          stx_after.stx_btime.tv_nsec == stx_before.stx_btime.tv_nsec,
          "btime unchanged across utimes (was %lld.%09u, now %lld.%09u)",
          (long long)stx_before.stx_btime.tv_sec, stx_before.stx_btime.tv_nsec,
          (long long)stx_after.stx_btime.tv_sec, stx_after.stx_btime.tv_nsec);
    CHECK(stx_after.stx_btime.tv_sec != 1000000000,
          "btime didn't get clobbered by utimes (now %lld)",
          (long long)stx_after.stx_btime.tv_sec);

    close(fd);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
