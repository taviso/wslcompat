/* Verify O_PATH works on WSL1.
 *
 * O_PATH (Linux 2.6.39+) opens a file as a "path reference" -- no
 * actual data fd, just an identifier you can use with the *at family
 * (openat, fstatat, unlinkat, etc.). Requires only search permission
 * on the path components, not read or write on the target itself.
 *
 * Useful if it works: lets the O_TMPFILE polyfill hold a dirfd to the
 * target directory without requiring read permission on it. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Can we open a directory with O_PATH at all? */
    int dirfd = open("/tmp", O_PATH | O_DIRECTORY);
    if (dirfd < 0) {
        printf("FAIL: open(/tmp, O_PATH | O_DIRECTORY): %s\n", strerror(errno));
        return 1;
    }
    CHECK(dirfd >= 0, "open(O_PATH | O_DIRECTORY) returned fd=%d", dirfd);

    /* Use it as a dirfd to create a file relative to it. This is the
     * exact pattern the O_TMPFILE polyfill needs. */
    char name[64];
    snprintf(name, sizeof(name), ".wslcompat-opath-%d", (int)getpid());
    int fd = openat(dirfd, name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        printf("  FAIL: openat(O_PATH dirfd, ...): %s\n", strerror(errno));
        failures++; subtests++;
        close(dirfd);
        return 1;
    }
    CHECK(fd >= 0, "openat(O_PATH dirfd, ...) returned fd=%d", fd);

    const char *payload = "ok";
    ssize_t w = write(fd, payload, strlen(payload));
    CHECK(w == (ssize_t)strlen(payload),
          "write through new fd returned %zd (want %zu)", w, strlen(payload));

    /* fstatat through the O_PATH dirfd. */
    struct stat st;
    int sr = fstatat(dirfd, name, &st, 0);
    CHECK(sr == 0 && st.st_size == w,
          "fstatat via O_PATH dirfd -> rc=%d st_size=%lld",
          sr, (long long)st.st_size);

    /* unlinkat through the O_PATH dirfd -- the cleanup path the
     * O_TMPFILE polyfill uses. */
    int ur = unlinkat(dirfd, name, 0);
    CHECK(ur == 0, "unlinkat via O_PATH dirfd -> rc=%d (errno=%s)",
          ur, ur ? strerror(errno) : "ok");

    close(fd);

    /* Confirm O_PATH is actually doing its job and not being silently
     * ignored. Per man open(2): read/write/fchmod/etc. on an O_PATH
     * fd return EBADF. A real O_RDONLY fd on a directory would return
     * EISDIR for read, not EBADF. */
    char ch;
    errno = 0;
    ssize_t rr = read(dirfd, &ch, 1);
    CHECK(rr == -1 && errno == EBADF,
          "read(O_PATH fd) -> rc=%zd errno=%s (want -1/EBADF)",
          rr, strerror(errno));

    close(dirfd);

    /* The other half: a directory we have search+write but no read on
     * should be openable with O_PATH but not with O_RDONLY. */
    char dir_template[] = "/tmp/wslcompat_opath_XXXXXX";
    if (!mkdtemp(dir_template)) {
        printf("  FAIL: mkdtemp: %s\n", strerror(errno));
        failures++; subtests++;
        return failures == 0 ? 0 : 1;
    }
    if (chmod(dir_template, 0300) != 0) {
        printf("  FAIL: chmod 0300: %s\n", strerror(errno));
        failures++; subtests++;
        rmdir(dir_template);
        return 1;
    }

    int rdfd = open(dir_template, O_RDONLY | O_DIRECTORY);
    int rd_err = errno;
    CHECK(rdfd == -1 && rd_err == EACCES,
          "O_RDONLY on read-less dir -> rc=%d errno=%s (want -1/EACCES)",
          rdfd, strerror(rd_err));
    if (rdfd >= 0) close(rdfd);

    int pathfd = open(dir_template, O_PATH | O_DIRECTORY);
    CHECK(pathfd >= 0,
          "O_PATH on read-less dir -> fd=%d (errno=%s)",
          pathfd, pathfd < 0 ? strerror(errno) : "ok");
    if (pathfd >= 0) close(pathfd);

    chmod(dir_template, 0700);
    rmdir(dir_template);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
