#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <err.h>

static void test_multiproc()
{
    char tmpfile[] = "/tmp/ofd_multi_XXXXXX";
    int fd;
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Subtest: cross-process conflicts ---\n");
    fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) err(EXIT_FAILURE, "parent fcntl");

    if (fork() == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");
        if (fcntl(fd2, F_OFD_SETLK, &fl) != -1) {
            errx(EXIT_FAILURE, "Error: Cross-process conflict not detected!");
        }
        printf("Child: Conflict detected.\n");
        exit(0);
    }
    int status; waitpid(-1, &status, 0);
    if (WEXITSTATUS(status) != 0) exit(1);
    
    unlink(tmpfile);
    close(fd);
}

static void test_crash_recovery()
{
    char tmpfile[] = "/tmp/ofd_crash_XXXXXX";
    int fd;
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Subtest: crash recovery (GC) ---\n");
    fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    pid_t pid = fork();
    if (pid == 0) {
        if (fcntl(fd, F_OFD_SETLK, &fl) == -1) err(EXIT_FAILURE, "child fcntl");
        raise(SIGKILL);
        _exit(1);
    }

    close(fd); // Release parent's reference to the OFD
    int status; waitpid(pid, &status, 0);

    int fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open");
    unlink(tmpfile);

    if (fcntl(fd2, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "Error: Could not acquire lock held by dead process!");
    }
    printf("Success: Stale lock garbage-collected.\n");
    close(fd2);
}

int main() {
    test_multiproc();
    test_crash_recovery();
    printf("all multiprocess tests pass\n");
    return 0;
}
