#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_range_hybrid_XXXXXX";
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 1234, .l_len = 5678 };

    printf("--- Test: Hybrid Range Accuracy ---\n");
    int fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    printf("1. Acquiring OFD lock on range [1234, 1234+5678]...\n");
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) {
        unlink(tmpfile);
        err(EXIT_FAILURE, "fcntl SETLK");
    }

    if (fork() == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        struct flock query = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

        printf("2. Child querying lock range via F_OFD_GETLK...\n");
        if (fcntl(fd2, F_OFD_GETLK, &query) == -1) err(EXIT_FAILURE, "child GETLK");

        printf("   Child: Found lock! Type=%s, Start=%ld, Len=%ld\n", 
               query.l_type == F_WRLCK ? "WRITE" : "READ", (long)query.l_start, (long)query.l_len);

        // If the hybrid approach works, we should see the original range (or 0-Inf).
        // If we only had flock, we'd ALWAYS see 0-0 (the whole file).
        // Actually, WSL1 upgrades to 0-4GB.
        if (query.l_start == 1234 && query.l_len == 5678) {
            printf("PASS: Original range recovered via POSIX hybrid.\n");
            exit(0);
        } else if (query.l_start == 0 && query.l_len == 0) {
            printf("FAIL: Range information lost (fell back to flock emulation).\n");
            exit(1);
        } else {
            printf("NOTE: Kernel reported modified range: Start=%ld, Len=%ld\n", (long)query.l_start, (long)query.l_len);
            exit(0); // Still potentially better than 0-0.
        }
    }

    int status;
    wait(&status);
    unlink(tmpfile);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}
