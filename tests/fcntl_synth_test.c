#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "testutil.h"

int main() {
    int fd = open("/proc/self/mem", O_RDONLY);
    // On native Linux, this succeeds.
    // On WSL1 without shim, this fails (ENOENT).
    // The test framework expects this to fail on unpatched WSL1,
    // and pass natively or with the shim.
    if (fd < 0) {
        return 1;
    }

    struct termios t;
    // Call ioctl. On Linux and WSL1, this should fail with ENOTTY or similar.
    int ioctl_res = ioctl(fd, TCGETS, &t);
    int err1 = errno;

    // Call F_SETFL
    int res = fcntl(fd, F_SETFL, 0);
    CHECK(res == 0 || (res == -1 && errno == EOPNOTSUPP), "fcntl F_SETFL should succeed or return EOPNOTSUPP");

    // Call ioctl again
    int ioctl_res2 = ioctl(fd, TCGETS, &t);
    int err2 = errno;

    // Under wslcompat's buggy implementation, F_SETFL clobbers O_PATH,
    // which changes the errno from EBADF to something else (e.g. ENOTTY).
    // On WSL1, allowing F_SETFL to reach the kernel on an O_PATH fd
    // appears to drop the O_PATH flag in F_GETFL, though the fd mostly
    // still behaves as an O_PATH fd. However, it alters the error code
    // returned by ioctl (from EBADF to e.g. ENOTTY), breaking the canary.
    CHECK(err1 == err2, "ioctl errno should not change after F_SETFL (was %d, now %d)", err1, err2);

    close(fd);
    TEST_REPORT();
}
