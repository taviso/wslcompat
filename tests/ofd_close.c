#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofdcloseXXXXXX";
    int fd1, fd2, fd3;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
        .l_pid    = 0,
    };

    fd1 = mkostemp(tmpfile, O_RDWR);
    fd2 = dup(fd1);
    fd3 = open(tmpfile, O_RDWR); // Different OFD
    unlink(tmpfile);

    printf("Acquiring lock on fd1...\n");
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "fcntl() F_OFD_SETLK failed on fd1");
    }

    printf("Closing fd2 (dup of fd1). Lock should NOT be lost.\n");
    close(fd2);

    printf("Attempting to acquire lock on fd3 (should FAIL if lock is still held by fd1)...\n");
    if (fcntl(fd3, F_OFD_SETLK, &fl) == -1) {
        if (errno == EAGAIN || errno == EACCES) {
            printf("As expected, failed to acquire lock on fd3: Lock is still held.\n");
        } else {
            err(EXIT_FAILURE, "fcntl lock fd3");
        }
    } else {
        err(EXIT_FAILURE, "error: Acquired lock on fd3! The lock was lost when fd2 was closed.\n");
    }

    close(fd1);
    close(fd3);
    return 0;
}
