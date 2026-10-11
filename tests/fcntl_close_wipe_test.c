/* POSIX fcntl record-lock close-wipe wart: when a process holds a
 * lock via fd1 and closes a *different* fd (fd2) referring to the
 * same file, the kernel drops every lock the process holds on that
 * file. OFD locks (F_OFD_SETLK) fix this; the wslcompat shim routes
 * POSIX fcntl through OFD under the hood, so the lock persists.
 *
 * Unshimmed (real Linux POSIX fcntl):
 *   close(fd2) wipes the lock acquired via fd1 -> child acquires
 *   conflicting lock -> wipe observed -> PASS.
 *
 * Shimmed (OFD under the hood):
 *   close(fd2) doesn't touch fd1's lock -> child is blocked -> FAIL.
 *   This is a known, intentional divergence: the OFD substitution is
 *   the whole point of the shim, and there is no plan to reintroduce
 *   POSIX wipe semantics. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <err.h>

#include "testutil.h"

enum { CHILD_WIPED = 0, CHILD_PERSISTED = 1, CHILD_ERROR = 2 };

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    char path[] = "/tmp/fcntl_close_wipe_XXXXXX";
    int fd1 = mkostemp(path, O_RDWR);
    if (fd1 < 0)
        err(EXIT_FAILURE, "mkostemp");

    int fd2 = open(path, O_RDWR);
    if (fd2 < 0)
        err(EXIT_FAILURE, "open fd2");

    unlink(path);

    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };
    if (fcntl(fd1, F_SETLK, &fl) < 0)
        err(EXIT_FAILURE, "fcntl(fd1, F_SETLK)");

    /* The wipe: closing an *unrelated* fd to the same file is what
     * POSIX says drops every lock the process holds on it. */
    close(fd2);

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd1);
        int fd3 = open(procpath, O_RDWR);
        if (fd3 < 0) {
            perror("child open");
            _exit(CHILD_ERROR);
        }
        if (fcntl(fd3, F_SETLK, &fl) == 0)
            _exit(CHILD_WIPED);
        if (errno == EAGAIN || errno == EACCES)
            _exit(CHILD_PERSISTED);
        perror("child fcntl");
        _exit(CHILD_ERROR);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0)
        err(EXIT_FAILURE, "waitpid");
    close(fd1);

    CHECK(WIFEXITED(status), "child must exit cleanly (status=0x%x)", status);

    if (WIFEXITED(status)) {
        int wstatus = WEXITSTATUS(status);
        CHECK(wstatus != CHILD_ERROR, "child must not encounter errors");
        CHECK(wstatus == CHILD_WIPED, "POSIX close-wipe must be observed (no OFD-style persistence)");
    }

    TEST_REPORT();
}
