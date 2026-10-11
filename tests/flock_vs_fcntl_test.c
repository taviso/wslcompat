#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/flock_vs_fcntl_XXXXXX";
    int fd;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };

    fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");
    unlink(tmpfile);

    printf("Testing if flock() and fcntl() conflict...\n");

    printf("Applying flock(LOCK_EX)...\n");
    if (flock(fd, LOCK_EX) == -1) {
        err(EXIT_FAILURE, "flock(LOCK_EX) failed");
    }

    printf("Attempting fcntl(F_SETLK, F_WRLCK)...\n");
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        if (errno == EAGAIN || errno == EACCES) {
            printf("RESULT: flock() and fcntl() CONFLICT.\n");
        } else {
            perror("fcntl");
        }
    } else {
        printf("RESULT: flock() and fcntl() are INDEPENDENT.\n");
    }

    close(fd);
    return 0;
}
