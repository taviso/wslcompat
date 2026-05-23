/* Verify that an O_TMPFILE-created fd can be linkat'd into the
 * filesystem -- the canonical "write to anonymous file then atomically
 * promote to permanent" pattern.
 *
 * Currently expected to FAIL on WSL1:
 *   - unpatched: open(O_TMPFILE) is EISDIR.
 *   - patched:   our polyfill creates a real file then unlinks it, so
 *                the inode has nlink == 0 and linkat returns EPERM.
 *                Faking nlink-zero-but-linkable isn't possible from
 *                userspace without bigger plumbing (fd tracking across
 *                dup/fork, close-on-cleanup, etc).
 *
 * This is an XFAIL marker -- if we ever land a richer O_TMPFILE
 * polyfill that supports linkat, the test will start passing and we'll
 * promote it to TESTS. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
    if (fd < 0) {
        printf("FAIL: open(O_TMPFILE): %s\n", strerror(errno));
        return 1;
    }

    const char *payload = "data";
    if (write(fd, payload, strlen(payload)) != (ssize_t)strlen(payload)) {
        printf("FAIL: write: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    char dst[64];
    snprintf(dst, sizeof(dst), "/tmp/wslcompat_linkat_test_%d", (int)getpid());
    unlink(dst);

    /* Try AT_EMPTY_PATH form first (modern). */
    if (linkat(fd, "", AT_FDCWD, dst, AT_EMPTY_PATH) != 0) {
        int e_empty = errno;
        /* Fall back to /proc/self/fd form (older). */
        char src[64];
        snprintf(src, sizeof(src), "/proc/self/fd/%d", fd);
        if (linkat(AT_FDCWD, src, AT_FDCWD, dst, AT_SYMLINK_FOLLOW) != 0) {
            printf("FAIL: linkat AT_EMPTY_PATH: %s\n", strerror(e_empty));
            printf("FAIL: linkat /proc/self/fd: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
    }

    /* Verify the linked path has the content. */
    int dst_fd = open(dst, O_RDONLY);
    if (dst_fd < 0) {
        printf("FAIL: open linked path: %s\n", strerror(errno));
        close(fd);
        unlink(dst);
        return 1;
    }
    char buf[16] = {0};
    ssize_t r = read(dst_fd, buf, sizeof(buf) - 1);
    close(dst_fd);
    close(fd);
    unlink(dst);

    if (r != (ssize_t)strlen(payload) || memcmp(buf, payload, r) != 0) {
        printf("FAIL: linked file content mismatch: got %zd bytes (%s)\n",
               r, buf);
        return 1;
    }

    printf("PASS: linkat into filesystem ok, content matches\n");
    return 0;
}
