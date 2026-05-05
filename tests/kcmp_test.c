#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/kcmp.h>
#include <errno.h>
#include <err.h>

static int kcmp(pid_t pid1, pid_t pid2, int type, unsigned long idx1, unsigned long idx2)
{
    return syscall(SYS_kcmp, pid1, pid2, type, idx1, idx2);
}

int main() {
    int fd1, fd2, fd3;
    char tmpfile[] = "/tmp/kcmptestXXXXXX";

    fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");

    fd2 = dup(fd1);
    if (fd2 == -1) err(EXIT_FAILURE, "dup");

    fd3 = open(tmpfile, O_RDWR);
    if (fd3 == -1) err(EXIT_FAILURE, "open");

    unlink(tmpfile);

    printf("Testing kcmp(KCMP_FILE)...\n");

    // fd1 and fd2 should be equal (same OFD)
    int res = kcmp(getpid(), getpid(), KCMP_FILE, fd1, fd2);
    if (res == -1) {
        if (errno == ENOSYS) {
            errx(EXIT_FAILURE, "kcmp() is not implemented on this kernel (ENOSYS).");
        }
        err(EXIT_FAILURE, "kcmp(fd1, fd2) failed");
    }

    if (res == 0) {
        printf("Success: kcmp correctly identified fd1 and fd2 as same OFD.\n");
    } else {
        errx(EXIT_FAILURE, "Failure: kcmp said fd1 and fd2 are different OFDs (%d).", res);
    }

    // fd1 and fd3 should be different (different OFD)
    res = kcmp(getpid(), getpid(), KCMP_FILE, fd1, fd3);
    if (res == -1) {
        err(EXIT_FAILURE, "kcmp(fd1, fd3) failed");
    }

    if (res != 0) {
        printf("Success: kcmp correctly identified fd1 and fd3 as different OFDs.\n");
    } else {
        errx(EXIT_FAILURE, "Failure: kcmp said fd1 and fd3 are the same OFD.");
    }

    close(fd1);
    close(fd2);
    close(fd3);

    return 0;
}
