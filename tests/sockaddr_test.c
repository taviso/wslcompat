#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "testutil.h"

static void test_unix_getsockopt(int sock_type, const char *type_name) {
    int fd;
    int protocol;
    int domain;
    int type;
    struct sockaddr un = {0};
    socklen_t len;

    STATUS("--- Testing AF_UNIX getsockopt for %s ---", type_name);

    fd = socket(AF_UNIX, sock_type, 0);
    CHECK(fd >= 0, "socket(%s) successful", type_name);
    if (fd < 0) return;

    len = sizeof(un);
    if (getsockname(fd, &un, &len) == 0) {
        CHECK(un.sa_family == AF_UNIX, "%s getsockname: sa_family == AF_UNIX", type_name);
    } else {
        CHECK(0, "%s getsockname failed", type_name);
    }

    len = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0) {
        CHECK(domain == AF_UNIX, "%s getsockopt SO_DOMAIN == AF_UNIX", type_name);
    } else {
        CHECK(0, "%s getsockopt SO_DOMAIN failed", type_name);
    }

    len = sizeof(protocol);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &len) == 0) {
        CHECK(protocol == 0, "%s getsockopt SO_PROTOCOL == 0", type_name);
    } else {
        CHECK(0, "%s getsockopt SO_PROTOCOL failed", type_name);
    }

    len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
        CHECK(type == sock_type, "%s getsockopt SO_TYPE == %s", type_name, type_name);
    } else {
        CHECK(0, "%s getsockopt SO_TYPE failed", type_name);
    }

    close(fd);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    test_unix_getsockopt(SOCK_STREAM, "SOCK_STREAM");
    test_unix_getsockopt(SOCK_DGRAM, "SOCK_DGRAM");

    TEST_REPORT();
}
