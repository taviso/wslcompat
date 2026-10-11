#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <err.h>

#define MAX_SWAPS 64
#define TEST_FDS (MAX_SWAPS + 4)

static void open_pty(int *master_fd, int *slave_fd)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s == -1) err(EXIT_FAILURE, "open slave");
    *master_fd = m;
    *slave_fd  = s;
}

static void set_raw(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) == -1) err(EXIT_FAILURE, "tcgetattr");
    t.c_lflag &= ~ICANON;
    if (tcsetattr(fd, TCSANOW, &t) == -1) err(EXIT_FAILURE, "tcsetattr");
}

int main(void)
{
    int masters[TEST_FDS];
    int slaves[TEST_FDS];
    struct pollfd fds[TEST_FDS];
    int original_fds[TEST_FDS];

    printf("Opening %d PTYs and setting to raw...\n", TEST_FDS);
    for (int i = 0; i < TEST_FDS; i++) {
        open_pty(&masters[i], &slaves[i]);
        set_raw(slaves[i]);
        fds[i].fd = slaves[i];
        fds[i].events = POLLIN;
        original_fds[i] = slaves[i];
    }

    printf("Calling poll()...\n");
    int r = poll(fds, TEST_FDS, 100);
    if (r < 0) err(EXIT_FAILURE, "poll");

    printf("Checking for corruption...\n");
    int failures = 0;
    for (int i = 0; i < TEST_FDS; i++) {
        if (fds[i].fd != original_fds[i]) {
            printf("  FAIL: fds[%d].fd changed from %d to %d\n",
                   i, original_fds[i], fds[i].fd);
            failures++;
        }
    }

    if (failures) {
        printf("%d failures detected!\n", failures);
        return EXIT_FAILURE;
    }

    printf("Success: no corruption detected.\n");
    return EXIT_SUCCESS;
}
