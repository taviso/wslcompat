/* Regression coverage for three edge cases in our statx shim that were
 * each fixed by hand after spotting them in code review:
 *
 *   1. read_btime_xattr with AT_FDCWD + "" used to call fgetxattr(-100)
 *      and EBADF-fail; should now call getxattr(".").
 *   2. read_btime_xattr with AT_EMPTY_PATH + AT_SYMLINK_NOFOLLOW used to
 *      call lgetxattr("/proc/self/fd/N") and read xattr off the procfs
 *      symlink (which doesn't carry user.* xattrs); should now call
 *      fgetxattr(dirfd) directly.
 *   3. is_mount_root with AT_EMPTY_PATH + non-empty pathname used to
 *      look at parent of dirfd instead of parent of pathname.
 *
 * Each subtest is structured to pass whenever the result is sensible for
 * the environment: the xattr-backed value when the shim is loaded, the
 * kernel-native value on a kernel that supports it. Fails only when
 * STATX_BTIME / STATX_ATTR_MOUNT_ROOT aren't supplied at all (i.e.
 * unpatched WSL1, where the kernel doesn't fill them and our shim isn't
 * there to either). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <linux/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef STATX_ATTR_MOUNT_ROOT
# define STATX_ATTR_MOUNT_ROOT 0x2000
#endif

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

#define BTIME_XATTR        "user.wslcompat.btime"
#define MARKER_SEC         1234567890LL
#define MARKER_VALUE       "1234567890.000000000"

static char tmpdir[64];
static char tmpfile_path[64];

static void cleanup(void)
{
    if (tmpfile_path[0]) unlink(tmpfile_path);
    if (tmpdir[0]) {
        if (chdir("/") == 0)
            rmdir(tmpdir);
    }
}

/* Subtest 1: statx(AT_FDCWD, "", AT_EMPTY_PATH) reads the cwd's btime
 * xattr correctly. Before the fix this routed to fgetxattr(-100). */
static void test_at_fdcwd_empty_path(void)
{
    printf("--- statx(AT_FDCWD, \"\", AT_EMPTY_PATH) reads cwd xattr ---\n");

    snprintf(tmpdir, sizeof(tmpdir), "/tmp/wslcompat_statx_%d", (int)getpid());
    rmdir(tmpdir);
    if (mkdir(tmpdir, 0700) != 0) {
        printf("  FAIL: mkdir: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    time_t mkdir_at = time(NULL);

    if (setxattr(tmpdir, BTIME_XATTR, MARKER_VALUE,
                 strlen(MARKER_VALUE), 0) != 0) {
        printf("  SKIP: setxattr on dir: %s (fs may not support xattrs)\n",
               strerror(errno));
        return;
    }
    if (chdir(tmpdir) != 0) {
        printf("  FAIL: chdir: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }

    struct statx stx = {0};
    if (statx(AT_FDCWD, "", AT_EMPTY_PATH, STATX_BTIME, &stx) != 0) {
        printf("  FAIL: statx: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }

    CHECK(stx.stx_mask & STATX_BTIME,
          "STATX_BTIME bit set in returned mask (mask=0x%x)", stx.stx_mask);
    CHECK(stx.stx_btime.tv_sec == MARKER_SEC ||
          (stx.stx_btime.tv_sec >= mkdir_at - 2 &&
           stx.stx_btime.tv_sec <= mkdir_at + 2),
          "btime=%lld is marker=%lld (shim) or near mkdir=%lld (native)",
          (long long)stx.stx_btime.tv_sec, MARKER_SEC, (long long)mkdir_at);
}

/* Subtest 2: statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) reads
 * btime via the fd's inode, not via lgetxattr on a procfs symlink. */
static void test_at_empty_path_nofollow(void)
{
    printf("--- statx(fd, \"\", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) ---\n");

    snprintf(tmpfile_path, sizeof(tmpfile_path),
             "/tmp/wslcompat_statxf_%d", (int)getpid());
    unlink(tmpfile_path);
    int fd = open(tmpfile_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        printf("  FAIL: open: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    time_t open_at = time(NULL);

    /* Force a deterministic xattr value (overwrites whatever the open
     * shim may have set, so we can tell shim-read from native-read). */
    if (fsetxattr(fd, BTIME_XATTR, MARKER_VALUE,
                  strlen(MARKER_VALUE), 0) != 0) {
        printf("  SKIP: fsetxattr: %s (fs may not support xattrs)\n",
               strerror(errno));
        close(fd);
        return;
    }

    struct statx stx = {0};
    if (statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
              STATX_BTIME, &stx) != 0) {
        printf("  FAIL: statx: %s\n", strerror(errno));
        failures++; subtests++;
        close(fd);
        return;
    }

    CHECK(stx.stx_mask & STATX_BTIME,
          "STATX_BTIME bit set (mask=0x%x)", stx.stx_mask);
    CHECK(stx.stx_btime.tv_sec == MARKER_SEC ||
          (stx.stx_btime.tv_sec >= open_at - 2 &&
           stx.stx_btime.tv_sec <= open_at + 2),
          "btime=%lld is marker=%lld (shim) or near open=%lld (native)",
          (long long)stx.stx_btime.tv_sec, MARKER_SEC, (long long)open_at);

    close(fd);
}

/* Subtest 3: is_mount_root respects pathname when AT_EMPTY_PATH is set
 * but the path is non-empty. Use /proc/self (not a mount root) opened
 * relative to / (which IS the root mount). Before the fix we'd look at
 * parent of dirfd (=/) and report /proc/self as a mount root. */
static void test_is_mount_root_emptypath_with_pathname(void)
{
    printf("--- is_mount_root: AT_EMPTY_PATH + non-empty pathname ---\n");

    int rootfd = open("/", O_RDONLY | O_DIRECTORY);
    if (rootfd < 0) {
        printf("  FAIL: open /: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }

    struct statx stx = {0};
    if (statx(rootfd, "/proc/self", AT_EMPTY_PATH,
              STATX_BASIC_STATS, &stx) != 0) {
        printf("  SKIP: statx on /proc/self: %s\n", strerror(errno));
        close(rootfd);
        return;
    }

    CHECK(stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT,
          "STATX_ATTR_MOUNT_ROOT bit populated in attributes_mask");
    CHECK(!(stx.stx_attributes & STATX_ATTR_MOUNT_ROOT),
          "/proc/self correctly NOT flagged as a mount root (attrs=0x%llx)",
          (unsigned long long)stx.stx_attributes);

    close(rootfd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    atexit(cleanup);

    test_at_fdcwd_empty_path();
    test_at_empty_path_nofollow();
    test_is_mount_root_emptypath_with_pathname();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
