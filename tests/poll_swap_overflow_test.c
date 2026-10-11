#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#define TEST_FDS 65

int main(void)
{
    struct pollfd fds[TEST_FDS];
    struct termios attrs;
    int master;
    int slave;
    int result;
    int i;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0)
        err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0)
        err(EXIT_FAILURE, "unlockpt");

    slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0)
        err(EXIT_FAILURE, "open slave");
    if (tcgetattr(slave, &attrs) != 0)
        err(EXIT_FAILURE, "tcgetattr");
    attrs.c_lflag &= ~ICANON;
    if (tcsetattr(slave, TCSANOW, &attrs) != 0)
        err(EXIT_FAILURE, "tcsetattr");

    for (i = 0; i < TEST_FDS; i++) {
        fds[i].fd = slave;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }

    result = poll(fds, TEST_FDS, 50);
    if (result != 0)
        errx(EXIT_FAILURE, "poll returned %d, expected timeout", result);

    for (i = 0; i < TEST_FDS; i++) {
        if (fds[i].revents != 0)
            errx(EXIT_FAILURE, "fds[%d].revents = %#x", i, fds[i].revents);
    }

    close(slave);
    close(master);
    return EXIT_SUCCESS;
}
