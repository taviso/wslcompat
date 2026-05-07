#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <sys/wait.h>

/**
 * Baseline Test: POSIX vs OFD Lock Conflict
 * 
 * This test is intended for a native Linux kernel (v3.15+).
 * It proves that OFD locks and POSIX locks are mutually exclusive.
 */

int main() {
    char tmpfile[] = "/tmp/linux_lock_XXXXXX";
    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0, // Whole file
    };

    printf("--- Baseline Linux: OFD vs POSIX Conflict Test ---\n");

    int fd1 = mkstemp(tmpfile);
    if (fd1 == -1) err(EXIT_FAILURE, "mkstemp");

    // 1. Parent acquires an OFD lock.
    printf("1. Parent: Acquiring F_OFD_SETLK...\n");
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) {
        if (errno == EINVAL) {
            fprintf(stderr, "FAIL: F_OFD_SETLK not supported by this kernel (expected on WSL1).\n");
        } else {
            perror("fcntl F_OFD_SETLK");
        }
        unlink(tmpfile);
        exit(EXIT_FAILURE);
    }

    // 2. Child attempts to acquire a POSIX lock on the same file.
    pid_t pid = fork();
    if (pid == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");

        struct flock fl2 = fl; // Same lock parameters, but for POSIX
        printf("2. Child: Attempting standard POSIX F_SETLK...\n");

        if (fcntl(fd2, F_SETLK, &fl2) == -1) {
            if (errno == EACCES || errno == EAGAIN) {
                printf("   SUCCESS: POSIX lock was correctly blocked by OFD lock.\n");
                exit(0);
            }
            err(EXIT_FAILURE, "child fcntl F_SETLK");
        }

        printf("   FAILURE: POSIX lock incorrectly acquired! (No conflict detected)\n");
        exit(1);
    }

    int status;
    waitpid(pid, &status, 0);
    unlink(tmpfile);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("\nRESULT: PASS. OFD and POSIX locks are part of the same system.\n");
        return 0;
    }
    
    printf("\nRESULT: FAIL. No conflict detected.\n");
    return 1;
}
