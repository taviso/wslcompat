#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#define termios asmtermios
#include <asm/termbits.h>
#undef termios
#include <sys/ioctl.h>
#include <termios.h>

#include "testutil.h"

int main(void)
{
    struct termios t;
    struct termios2 t2;
    char c;

    alarm(2);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master == -1)
        err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) == -1)
        err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) == -1)
        err(EXIT_FAILURE, "unlockpt");

    int s1 = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (s1 == -1)
        err(EXIT_FAILURE, "open slave");

    int s2 = dup(s1);
    if (s2 == -1)
        err(EXIT_FAILURE, "dup");

    if (tcgetattr(s2, &t) == -1)
        err(EXIT_FAILURE, "tcgetattr");
    cfmakeraw(&t);
    if (tcsetattr(s2, TCSANOW, &t) == -1)
        err(EXIT_FAILURE, "tcsetattr");

    // Read-only query on s1 must not wrap s1 with a stale nonblock=0 cache.
    CHECK(ioctl(s1, TCGETS2, &t2) == 0, "ioctl(TCGETS2)");

    if (fcntl(s2, F_SETFL, O_NONBLOCK) == -1)
        err(EXIT_FAILURE, "fcntl(O_NONBLOCK)");

    CHECK(read(s1, &c, 1) == -1 && errno == EAGAIN,
          "non-blocking read(s1) returns EAGAIN");

    close(s1);
    close(s2);
    close(master);
    TEST_REPORT();
}
