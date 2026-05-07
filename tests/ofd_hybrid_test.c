#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_hybrid_XXXXXX";
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Test: Hybrid Emulation Reliability ---\n");
    int fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");

    printf("1. Acquiring OFD lock on fd1 (PID %d)...\n", getpid());
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) err(EXIT_FAILURE, "fcntl SETLK");

    printf("2. Querying lock from same process, different FD (but same OFD via dup)...\n");
    int fd2 = dup(fd1);
    struct flock q1 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd2, F_OFD_GETLK, &q1) == -1) err(EXIT_FAILURE, "GETLK q1");
    printf("   Result: %s (Expected: UNLOCKED - F_OFD_GETLK ignores own OFD)\n", 
           q1.l_type == F_UNLCK ? "UNLOCKED" : "LOCKED");

    printf("3. Querying lock from same process, DIFFERENT OFD (via open)...\n");
    int fd3 = open(tmpfile, O_RDWR);
    struct flock q2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd3, F_OFD_GETLK, &q2) == -1) err(EXIT_FAILURE, "GETLK q2");
    printf("   Result: %s (Expected: LOCKED - Different OFDs conflict)\n", 
           q2.l_type == F_UNLCK ? "UNLOCKED" : "LOCKED");

    printf("4. Querying lock from DIFFERENT process...\n");
    if (fork() == 0) {
        int fd4 = open(tmpfile, O_RDWR);
        struct flock q3 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        if (fcntl(fd4, F_OFD_GETLK, &q3) == -1) err(EXIT_FAILURE, "GETLK q3");
        printf("   Child Result: %s\n", q3.l_type == F_UNLCK ? "UNLOCKED" : "LOCKED");
        exit(q3.l_type == F_WRLCK ? 0 : 1);
    }
    int status; wait(&status);
    
    unlink(tmpfile);
    return WEXITSTATUS(status);
}
