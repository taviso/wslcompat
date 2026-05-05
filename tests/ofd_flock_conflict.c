#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_flock_conflict_XXXXXX";
    int fd1, fd2;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };

    fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");
    
    fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open");

    unlink(tmpfile);

    printf("Testing independence of flock() and F_OFD_SETLK...\n");

    printf("Acquiring flock(LOCK_EX) on fd1...\n");
    if (flock(fd1, LOCK_EX) == -1) {
        err(EXIT_FAILURE, "flock(fd1) failed");
    }

    printf("Attempting F_OFD_SETLK (whole file) on fd2...\n");
    printf("NOTE: On Linux, this should succeed. In the current shim, it will fail.\n");
    
    if (fcntl(fd2, F_OFD_SETLK, &fl) == 0) {
        printf("Success: flock() and F_OFD_SETLK are independent.\n");
    } else {
        if (errno == EAGAIN || errno == EACCES) {
            errx(EXIT_FAILURE, "Failure: flock() and F_OFD_SETLK incorrectly conflicted.");
        } else {
            err(EXIT_FAILURE, "fcntl F_OFD_SETLK");
        }
    }

    close(fd1);
    close(fd2);

    return 0;
}
