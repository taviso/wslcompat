#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "posix_test_file";
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Test: POSIX vs POSIX Conflict (Raw, local file) ---\n");
    int fd1 = open(tmpfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd1 == -1) err(EXIT_FAILURE, "open");

    printf("1. Parent: Acquiring standard POSIX F_SETLK...\n");
    if (fcntl(fd1, F_SETLK, &fl) == -1) err(EXIT_FAILURE, "parent fcntl");

    if (fork() == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        struct flock fl2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        
        printf("2. Child: Attempting standard F_SETLK...\n");
        if (fcntl(fd2, F_SETLK, &fl2) == -1) {
            if (errno == EACCES || errno == EAGAIN) {
                printf("   PASS: Child correctly blocked by Parent's POSIX lock.\n");
                exit(0);
            }
            err(EXIT_FAILURE, "child fcntl");
        }

        printf("   FAIL: Child incorrectly acquired POSIX lock! (No conflict detected)\n");
        exit(1);
    }

    int status;
    wait(&status);
    unlink(tmpfile);
    
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("POSIX vs POSIX conflict test passed.\n");
        return 0;
    }
    return 1;
}
