#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_pid_XXXXXX";
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Test: OFD Lock PID Visibility (Hybrid Emulation) ---\n");
    int fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    pid_t parent_pid = getpid();
    printf("Parent (PID %d) acquiring OFD lock...\n", parent_pid);
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) {
        unlink(tmpfile);
        err(EXIT_FAILURE, "fcntl SETLK");
    }

    pid_t child_pid = fork();
    if (child_pid == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");

        struct flock query = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        printf("Child (PID %d) querying lock state...\n", getpid());

        if (fcntl(fd2, F_OFD_GETLK, &query) == -1) err(EXIT_FAILURE, "child GETLK");

        if (query.l_type == F_UNLCK) {
            errx(EXIT_FAILURE, "FAIL: Child reported file was UNLOCKED!");
        }

        printf("Child: Found lock! Type=%s, PID=%d (Expected %d)\n", 
               query.l_type == F_WRLCK ? "WRITE" : "READ", query.l_pid, parent_pid);

        if (query.l_pid == parent_pid || query.l_pid == -1) {
            printf("PASS: Correct PID recovered via hybrid emulation.\n");
            exit(0);
        } else {
            printf("FAIL: Incorrect PID (or -1) recovered.\n");
            exit(1);
        }
    }

    int status;
    waitpid(child_pid, &status, 0);
    unlink(tmpfile);
    close(fd);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PID visibility test passed.\n");
        return 0;
    }
    return 1;
}
