#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

int main(int argc, char **argv) {
    int fd;
    int protocol;
    int domain;
    int type;
    struct sockaddr un = {0};
    socklen_t len;

    // Create socket
    fd = socket(AF_UNIX, SOCK_STREAM, 0);

    // Get AF
    len = sizeof(un);
    if (getsockname(fd, &un, &len) == 0) {
        if (un.sa_family == AF_UNIX) {
            printf("socket is AF_UNIX\n");
        }
    } else {
        err(EXIT_FAILURE, "getsockname failed");
    }

    // Get AF
    len = sizeof(domain);
    if(getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0) {
        if (domain == AF_UNIX) {
            printf("socket is AF_UNIX\n");
        }
    } else {
        err(EXIT_FAILURE, "getsockopt SOL_SOCKET failed");
    }

    // Get protocol
    len = sizeof(protocol);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &len) == -1) {
        err(EXIT_FAILURE, "getsockopt SO_PROTOCOL failed");
    } else {
        printf("SO_PROTOCOL for AF_UNIX socket: %d\n", protocol);
    }

    // Get type
    len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == -1) {
        err(EXIT_FAILURE, "getsockopt SO_TYPE failed");
    } else {
        printf("SO_TYPE for AF_UNIX socket: %d\n", protocol);
    }

#if 0
    // Check SO_TIMESTAMP
    int timestamp;
    len = sizeof(timestamp);
    if (getsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &timestamp, &len) == -1) {
        err(EXIT_FAILURE, "getsockopt SO_TIMESTAMP failed");
    } else {
        printf("SO_TIMESTAMP is %d\n", timestamp);
    }

    timestamp = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &timestamp, sizeof(timestamp)) == -1) {
        err(EXIT_FAILURE, "setsockopt SO_TIMESTAMP failed");
    } else {
        printf("Successfully set SO_TIMESTAMP to 1\n");
    }
#endif

    close(fd);
    return 0;
}
