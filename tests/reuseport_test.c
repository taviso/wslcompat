/* Exercise SO_REUSEPORT for TCP and UDP. With the option set on both
 * sockets before bind, two sockets should be able to share the same
 * port. Without it, the second bind should fail with EADDRINUSE. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "testutil.h"

/* Open a socket of the requested type, optionally set SO_REUSEPORT or
 * SO_REUSEADDR, then bind to 127.0.0.1:port (port=0 means kernel-assigned).
 * On success returns the fd and fills *out_port. On failure returns -1
 * with errno set. */
static int open_and_bind(int type, int opt, in_port_t port,
                         in_port_t *out_port)
{
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;

    if (opt) {
        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, opt, &on, sizeof(on)) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (out_port) {
        socklen_t len = sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        *out_port = ntohs(sa.sin_port);
    }
    return fd;
}

static const char *opt_name(int opt)
{
    if (opt == SO_REUSEPORT) return "SO_REUSEPORT";
    if (opt == SO_REUSEADDR) return "SO_REUSEADDR";
    return "none";
}

static void test_share(int type, int opt, const char *label)
{
    STATUS("--- %s with %s ---", label, opt_name(opt));
    in_port_t port;
    int a = open_and_bind(type, opt, 0, &port);
    if (a < 0) {
        CHECK(0, "first bind: %s", strerror(errno));
        return;
    }
    int b = open_and_bind(type, opt, port, NULL);
    if (b < 0) {
        CHECK(0, "second bind to port %u: %s",
              (unsigned)port, strerror(errno));
        close(a);
        return;
    }
    CHECK(1, "%s/%s: two sockets sharing port %u",
          label, opt_name(opt), (unsigned)port);
    close(a);
    close(b);
}

static void test_no_share(int type, const char *label)
{
    STATUS("--- %s without SO_REUSEPORT ---", label);
    in_port_t port;
    int a = open_and_bind(type, 0, 0, &port);
    if (a < 0) {
        CHECK(0, "first bind: %s", strerror(errno));
        return;
    }
    errno = 0;
    int b = open_and_bind(type, 0, port, NULL);
    int saved_errno = errno;
    if (b >= 0) {
        CHECK(0, "%s: second bind unexpectedly succeeded", label);
        close(a);
        close(b);
        return;
    }
    CHECK(saved_errno == EADDRINUSE,
          "%s: second bind rejected with %s (want EADDRINUSE)",
          label, strerror(saved_errno));
    close(a);
}

/* Can we round-trip SO_REUSEPORT via getsockopt? If yes, a polyfill
 * could detect "app asked for REUSEPORT" after the fact. */
static void test_getsockopt_roundtrip(int type, const char *label)
{
    STATUS("--- %s getsockopt(SO_REUSEPORT) round-trip ---", label);
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        CHECK(0, "socket: %s", strerror(errno));
        return;
    }

    int got = -1;
    socklen_t len = sizeof(got);
    if (getsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &got, &len) != 0) {
        CHECK(0, "getsockopt before set: %s", strerror(errno));
        close(fd);
        return;
    }
    CHECK(got == 0, "%s: initial getsockopt = %d (want 0)", label, got);

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) {
        CHECK(0, "setsockopt(1): %s", strerror(errno));
        close(fd);
        return;
    }

    got = -1;
    len = sizeof(got);
    if (getsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &got, &len) != 0) {
        CHECK(0, "getsockopt after set: %s", strerror(errno));
        close(fd);
        return;
    }
    CHECK(got != 0, "%s: getsockopt after set = %d (want non-zero)", label, got);

    close(fd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_getsockopt_roundtrip(SOCK_STREAM, "TCP");
    test_getsockopt_roundtrip(SOCK_DGRAM,  "UDP");
    test_share(SOCK_STREAM, SO_REUSEPORT, "TCP");
    test_share(SOCK_DGRAM,  SO_REUSEPORT, "UDP");
    test_share(SOCK_STREAM, SO_REUSEADDR, "TCP");
    test_share(SOCK_DGRAM,  SO_REUSEADDR, "UDP");
    test_no_share(SOCK_STREAM, "TCP");
    test_no_share(SOCK_DGRAM,  "UDP");

    TEST_REPORT();
}
