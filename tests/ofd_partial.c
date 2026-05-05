#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_partial_XXXXXX";
    int fd1, fd2;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 10, // Partial lock
    };

    fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");

    fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open");

    unlink(tmpfile);

    printf("Attempting to acquire partial lock (0-10) on fd1...\n");
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "fcntl() F_OFD_SETLK failed on fd1");
    }

    printf("Attempting to acquire conflicting partial lock (0-10) on fd2...\n");
    // This should fail because fd1 and fd2 are different OFDs.
    // In the current shim, it falls back to F_SETLK which is per-process,
    // so it will incorrectly succeed.
    if (fcntl(fd2, F_OFD_SETLK, &fl) == -1) {
        if (errno == EAGAIN || errno == EACCES) {
            printf("As expected, failed to acquire lock on fd2.\n");
        } else {
            err(EXIT_FAILURE, "fcntl lock fd2");
        }
    } else {
        errx(EXIT_FAILURE, "error: Acquired lock on fd2 when it should have failed.");
    }

    close(fd1);
    close(fd2);

    return 0;
}
