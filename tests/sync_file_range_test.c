/* sync_file_range(2) on a plain regular file. Listed in WSL1's
 * SYSCALLS.md but the kernel actually returns ENOSYS, so this is
 * XFAIL on WSL1 until we polyfill it.
 *
 * Polyfill plan: shim sync_file_range to fall back to fsync (or
 * fdatasync if SYNC_FILE_RANGE_WRITE is the only flag set) when
 * sym_next returns ENOSYS. Once landed, this test graduates to TESTS. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    char path[] = "/tmp/sfr_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) err(EXIT_FAILURE, "mkstemp");
    unlink(path);

    const char seed[] = "0123456789abcdef";
    if (write(fd, seed, sizeof(seed) - 1) != (ssize_t)(sizeof(seed) - 1))
        err(EXIT_FAILURE, "seed write");

    int r = sync_file_range(fd, 0, sizeof(seed) - 1,
                            SYNC_FILE_RANGE_WAIT_BEFORE |
                            SYNC_FILE_RANGE_WRITE |
                            SYNC_FILE_RANGE_WAIT_AFTER);
    if (r != 0) {
        printf("FAIL: sync_file_range -> %d %s\n", r, strerror(errno));
        return 1;
    }

    printf("PASS: sync_file_range succeeded\n");
    return 0;
}
