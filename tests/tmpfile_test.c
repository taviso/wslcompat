#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/xattr.h>
#include "testutil.h"

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
    CHECK(fd >= 0, "open(/tmp, O_TMPFILE | O_RDWR) returned fd=%d (errno=%d)", fd, errno);

    /* Anonymous file: should have zero links. */
    struct stat st = {0};
    int f_ret = fstat(fd, &st);
    CHECK(f_ret == 0 && st.st_nlink == 0, 
          "fstat and unlinked (errno=%d, nlink=%lu)", errno, (unsigned long)st.st_nlink);

    /* Round-trip a write/read to confirm the fd is usable. */
    const char *payload = "abcdef";
    ssize_t w = write(fd, payload, strlen(payload));
    CHECK(w == (ssize_t)strlen(payload), "write returned %zd (want %zu)", w, strlen(payload));

    off_t l_ret = lseek(fd, 0, SEEK_SET);
    CHECK(l_ret != (off_t)-1, "lseek (errno=%d)", errno);

    char buf[16] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    CHECK(r == w && memcmp(buf, payload, w) == 0, "read returned %zd bytes (%.*s)", r, (int)r, buf);

    struct statx stx = {0};
    int sx_ret = statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx);
    CHECK(sx_ret == 0 && (stx.stx_mask & STATX_BTIME), 
          "statx btime (errno=%d, mask=0x%x)", errno, stx.stx_mask);

    if (fd >= 0) {
        close(fd);
    }
    
    TEST_REPORT();
}
