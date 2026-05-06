#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_block_XXXXXX";
    int fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Test: F_OFD_SETLKW blocking behavior ---\n");
    printf("Parent: Acquiring lock...\n");
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) err(EXIT_FAILURE, "parent fcntl");

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Child process
        int fd2 = open(tmpfile, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");

        printf("Child: Attempting F_OFD_SETLKW (should block)...\n");
        time_t start = time(NULL);
        
        if (fcntl(fd2, F_OFD_SETLKW, &fl) == -1) {
            perror("Child: fcntl F_OFD_SETLKW failed");
            exit(1);
        }

        time_t end = time(NULL);
        printf("Child: Successfully acquired lock after %ld seconds.\n", (long)(end - start));
        
        if ((end - start) < 2) {
            fprintf(stderr, "Child: Failure: Did not block for long enough!\n");
            exit(1);
        }
        exit(0);
    }

    printf("Parent: Holding lock for 3 seconds...\n");
    sleep(3);

    printf("Parent: Releasing lock...\n");
    fl.l_type = F_UNLCK;
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) err(EXIT_FAILURE, "parent release");

    int status;
    waitpid(pid, &status, 0);

    unlink(tmpfile);
    close(fd);

    if (WEXITSTATUS(status) != 0) {
        errx(EXIT_FAILURE, "Blocking test failed.");
    }

    printf("all blocking tests pass\n");
    return 0;
}
