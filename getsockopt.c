#define _GNU_SOURCE
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>

static int (*sym_getsockopt)(int, int, int, void *, socklen_t *);

static void __attribute__((constructor)) init(void)
{
    sym_getsockopt = dlsym(RTLD_NEXT, "getsockopt");
    return;
}

int getsockopt(int sockfd,
               int level,
               int optname,
               void *optval,
               socklen_t *optlen)
{
    struct sockaddr sa = {0};
    socklen_t len = sizeof(sa);

    // Pass through the request.
    int result = sym_getsockopt(sockfd, level, optname, optval, optlen);

    // If it worked, we're good.
    if (result != -1 || errno != EINVAL)
        return result;

    // We only handle sockets.
    if (level != SOL_SOCKET || !optlen || !optval)
        return result;

    // Test if this is an AF_UNIX socket.
    if (getsockname(sockfd, &sa, &len) != 0)
        return result;

    if (sa.sa_family != AF_UNIX)
        return result;

    // Okay, see if this is one we can easily emulate.
    if (optname == SO_DOMAIN && *optlen >= sizeof(sa.sa_family)) {
        memcpy(optval, &sa.sa_family, sizeof sa.sa_family);
        return 0;
    }

    // This one is always 0 for AF_UNIX
    if (optname == SO_PROTOCOL && *optlen >= sizeof(int)) {
        memset(optval, 0, sizeof(int));
        return 0;
    }

    return result;
}
