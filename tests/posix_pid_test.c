#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/posix_pid_test_XXXXXX";
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    int fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    pid_t parent_pid = getpid();
    if (fcntl(fd, F_SETLK, &fl) == -1) err(EXIT_FAILURE, "fcntl SETLK");

    pid_t child_pid = fork();
    if (child_pid == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        struct flock query = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        if (fcntl(fd2, F_GETLK, &query) == -1) err(EXIT_FAILURE, "child GETLK");
        printf("POSIX GETLK PID: %d (Expected %d)\n", query.l_pid, parent_pid);
        if (query.l_pid == parent_pid)
            exit(0);
        exit(1);
    }

    int status;
    wait(&status);
    unlink(tmpfile);
    return WEXITSTATUS(status);
}
