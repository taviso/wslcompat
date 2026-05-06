#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <sys/wait.h>

static void test_partial_conflicts()
{
    char tmpfile[] = "/tmp/ofd_range_XXXXXX";
    int fd1, fd2;
    struct flock fl1 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 10 };
    struct flock fl2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 5, .l_len = 5 };

    printf("--- Subtest: partial range conflicts ---\n");
    fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");
    
    fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open");
    unlink(tmpfile);

    printf("Acquiring lock (0-10) on fd1...\n");
    if (fcntl(fd1, F_OFD_SETLK, &fl1) == -1) err(EXIT_FAILURE, "fcntl fd1");

    printf("Attempting conflicting lock (5-10) on fd2...\n");
    if (fcntl(fd2, F_OFD_SETLK, &fl2) != -1) {
        errx(EXIT_FAILURE, "Error: Conflicting partial lock incorrectly succeeded!");
    }
    printf("Success: Conflict correctly detected.\n");

    close(fd1);
    close(fd2);
}

static void test_merging()
{
    char tmpfile[] = "/tmp/ofd_merge_XXXXXX";
    int fd;
    struct flock fl1 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 10 };
    struct flock fl2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 10 };
    struct flock query = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Subtest: lock merging ---\n");
    fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    if (fcntl(fd, F_OFD_SETLK, &fl1) == -1) err(EXIT_FAILURE, "fcntl 1");
    if (fcntl(fd, F_OFD_SETLK, &fl2) == -1) err(EXIT_FAILURE, "fcntl 2");

    printf("Verifying locks merged into 0-20 via child query...\n");
    if (fork() == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        struct flock q = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 5, .l_len = 1 };
        if (fcntl(fd2, F_OFD_GETLK, &q) == -1) err(EXIT_FAILURE, "fcntl query");
        
        if (q.l_type == F_WRLCK && q.l_start == 0 && q.l_len == 20) {
            printf("Child Success: Locks merged into 0-20.\n");
            exit(0);
        }
        fprintf(stderr, "Child Failure: Merged lock report was wrong (start=%ld, len=%ld)\n", (long)q.l_start, (long)q.l_len);
        exit(1);
    }
    int status; waitpid(-1, &status, 0);
    if (WEXITSTATUS(status) != 0) exit(1);

    unlink(tmpfile);
    close(fd);
}

int main() {
    test_partial_conflicts();
    test_merging();
    printf("all range tests pass\n");
    return 0;
}
