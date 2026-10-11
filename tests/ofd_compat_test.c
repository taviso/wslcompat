#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <err.h>

static void test_flock_independence()
{
    char tmpfile[] = "/tmp/ofd_flock_XXXXXX";
    int fd1, fd2;
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Subtest: flock vs OFD independence ---\n");
    fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");
    fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open");
    unlink(tmpfile);

    printf("Acquiring flock(LOCK_EX) on fd1...\n");
    if (flock(fd1, LOCK_EX) == -1) err(EXIT_FAILURE, "flock");

    printf("Attempting OFD lock on fd2 (should succeed)...\n");
    if (fcntl(fd2, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "Failure: flock incorrectly blocked OFD lock");
    }
    printf("Success: Independent owners.\n");

    close(fd1);
    close(fd2);
}

static void test_posix_conflict()
{
    char tmpfile[] = "/tmp/ofd_posix_XXXXXX";
    int fd;
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Subtest: POSIX vs OFD conflict ---\n");
    fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");
    unlink(tmpfile);

    printf("Acquiring POSIX lock (F_SETLK)...\n");
    if (fcntl(fd, F_SETLK, &fl) == -1) err(EXIT_FAILURE, "F_SETLK");

    printf("Attempting OFD lock (F_OFD_SETLK) on same FD (should fail)...\n");
    if (fcntl(fd, F_OFD_SETLK, &fl) == 0) {
        errx(EXIT_FAILURE, "Failure: POSIX lock incorrectly allowed OFD lock");
    }
    printf("Success: Conflict detected.\n");

    close(fd);
}

int main() {
    test_flock_independence();
    test_posix_conflict();
    printf("all compatibility tests pass\n");
    return 0;
}
