#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>
#include <err.h>

#define HOLD_MS    1000
#define TOLERANCE  500

static long elapsed_ms(struct timeval *t0, struct timeval *t1)
{
    return (t1->tv_sec  - t0->tv_sec)  * 1000
         + (t1->tv_usec - t0->tv_usec) / 1000;
}

int main() {
    char tmpfile[] = "/tmp/ofd_block_XXXXXX";
    int fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Test: F_OFD_SETLKW blocking behavior ---\n");
    printf("Parent: Acquiring lock...\n");
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) {
        unlink(tmpfile);
        err(EXIT_FAILURE, "parent fcntl");
    }

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Child process
        int fd2 = open(tmpfile, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");

        printf("Child: Attempting F_OFD_SETLKW (should block)...\n");
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);

        if (fcntl(fd2, F_OFD_SETLKW, &fl) == -1) {
            perror("Child: fcntl F_OFD_SETLKW failed");
            exit(1);
        }

        gettimeofday(&t1, NULL);
        long ms = elapsed_ms(&t0, &t1);
        printf("Child: Successfully acquired lock after %ld ms.\n", ms);

        // Check the blocking duration matches the parent's hold window
        // to within +/- TOLERANCE ms. Too short means we didn't block;
        // too long means something hung.
        if (ms < HOLD_MS - TOLERANCE || ms > HOLD_MS + TOLERANCE) {
            fprintf(stderr, "Child: Failure: blocked for %ld ms, "
                            "expected ~%d +/- %d ms\n",
                            ms, HOLD_MS, TOLERANCE);
            exit(1);
        }
        exit(0);
    }

    printf("Parent: Holding lock for %d ms...\n", HOLD_MS);
    usleep(HOLD_MS * 1000);

    printf("Parent: Releasing lock...\n");
    fl.l_type = F_UNLCK;
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) {
        unlink(tmpfile);
        err(EXIT_FAILURE, "parent release");
    }

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
