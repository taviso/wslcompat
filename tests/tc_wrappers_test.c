#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "testutil.h"

int main(int argc, char **argv)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m != -1, "posix_openpt");
    CHECK(grantpt(m) != -1, "grantpt");
    CHECK(unlockpt(m) != -1, "unlockpt");
    char *name = ptsname(m);
    CHECK(name != NULL, "ptsname");
    
    int s = open(name, O_RDWR | O_NOCTTY);
    CHECK(s != -1, "open slave");

    // Get the initial state.
    struct termios t;
    CHECK(tcgetattr(s, &t) == 0, "tcgetattr natively");
    
    // Changing the state will trigger wslcompat's tty wrap via TCSETS.
    t.c_lflag &= ~ICANON;
    CHECK(tcsetattr(s, TCSANOW, &t) == 0, "tcsetattr (triggers wslcompat wrap)");
    
    // Now verify the wrappers work on the synthetic wrapped FD.
    CHECK(isatty(s) == 1, "isatty returns 1 on wrapped fd");
    
    struct termios t2;
    CHECK(tcgetattr(s, &t2) == 0, "tcgetattr on wrapped fd");
    CHECK((t2.c_lflag & ICANON) == 0, "tcgetattr retrieves correct wrapped state");
    
    char *tname = ttyname(s);
    STATUS("ttyname = %s", tname ? tname : "NULL");
    CHECK(tname != NULL, "ttyname works on wrapped fd");
    CHECK(strcmp(tname, name) == 0, "ttyname matches actual ptsname");
    
    char buf[128];
    int r = ttyname_r(s, buf, sizeof(buf));
    CHECK(r == 0, "ttyname_r works on wrapped fd");
    CHECK(strcmp(buf, name) == 0, "ttyname_r matches actual ptsname");
    
    // Test the remaining ioctl-bypassing wrappers to ensure they don't hit the EBADF canary.
    // They may fail with ENOTTY (not a controlling tty), but they must properly route.
    r = tcgetpgrp(s);
    CHECK(r != -1 || errno != EBADF, "tcgetpgrp routed correctly");

    r = tcsetpgrp(s, getpgrp());
    CHECK(r != -1 || errno != EBADF, "tcsetpgrp routed correctly");
    
    r = tcgetsid(s);
    CHECK(r != -1 || errno != EBADF, "tcgetsid routed correctly");
    
    r = tcdrain(s);
    CHECK(r != -1 || errno != EBADF, "tcdrain routed correctly");
    
    r = tcflush(s, TCIOFLUSH);
    CHECK(r != -1 || errno != EBADF, "tcflush routed correctly");
    
    r = tcflow(s, TCOON);
    CHECK(r != -1 || errno != EBADF, "tcflow routed correctly");
    
    r = tcsendbreak(s, 0);
    CHECK(r != -1 || errno != EBADF, "tcsendbreak routed correctly");

    close(s);
    close(m);
    
    TEST_REPORT();
}
