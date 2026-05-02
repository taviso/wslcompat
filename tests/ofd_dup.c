#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofddupXXXXXX";
    int fd1, fd2;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
        .l_pid    = 0,
    };

    fd1 = mkostemp(tmpfile, O_RDWR);
    fd2 = dup(fd1);
    unlink(tmpfile);

    printf("Acquiring lock on fd1...\n");
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "fcntl() F_OFD_SETLK failed on fd1");
    }

    printf("Acquiring lock on fd2 (should succeed because same OFD)...\n");
    if (fcntl(fd2, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "Error: Failed to acquire lock on fd2 (dup of fd1)\n");
    } else {
        printf("Successfully acquired lock on fd2 (as expected).\n");
    }

    close(fd1);
    close(fd2);
    return 0;
}
