/* Verify open(dir, O_TMPFILE | O_RDWR, mode) works:
 *   - returns a valid fd
 *   - the fd is read/write capable
 *   - the file has no name (st_nlink == 0)
 *
 * On WSL1 the kernel doesn't recognize __O_TMPFILE and returns EISDIR
 * on the bare directory open; the open shim polyfills via mkstemp +
 * immediate unlink. */
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

    int fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
    if (fd < 0) {
        printf("FAIL: open(/tmp, O_TMPFILE | O_RDWR): %s\n", strerror(errno));
        return 1;
    }
    CHECK(fd >= 0, "open(O_TMPFILE) returned fd=%d", fd);

    /* Anonymous file: should have zero links. */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        printf("  FAIL: fstat: %s\n", strerror(errno));
        failures++; subtests++;
        close(fd);
        return 1;
    }
    CHECK(st.st_nlink == 0,
          "st_nlink=%lu (want 0 -- file is unlinked)", (unsigned long)st.st_nlink);

    /* Round-trip a write/read to confirm the fd is usable. */
    const char *payload = "abcdef";
    ssize_t w = write(fd, payload, strlen(payload));
    CHECK(w == (ssize_t)strlen(payload),
          "write returned %zd (want %zu)", w, strlen(payload));

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        printf("  FAIL: lseek: %s\n", strerror(errno));
        failures++; subtests++;
        close(fd);
        return 1;
    }

    char buf[16] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    CHECK(r == w && memcmp(buf, payload, w) == 0,
          "read returned %zd bytes (%.*s)", r, (int)r, buf);

    close(fd);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
