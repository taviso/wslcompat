#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "testutil.h"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    STATUS("--- Testing AF_UNIX SO_TIMESTAMP ---");

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0, "socket(AF_UNIX) successful");
    if (fd < 0) return 1;

    int timestamp = 0;
    socklen_t len = sizeof(timestamp);
    if (getsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &timestamp, &len) == 0) {
        CHECK(1, "getsockopt SO_TIMESTAMP successful (value: %d)", timestamp);
    } else {
        CHECK(0, "getsockopt SO_TIMESTAMP failed");
    }

    timestamp = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &timestamp, sizeof(timestamp)) == 0) {
        CHECK(1, "setsockopt SO_TIMESTAMP successful");
    } else {
        CHECK(0, "setsockopt SO_TIMESTAMP failed");
    }

    close(fd);
    TEST_REPORT();
}
