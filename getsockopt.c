#define _GNU_SOURCE
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(getsockopt);


int getsockopt(int sockfd,
               int level,
               int optname,
               void *optval,
               socklen_t *optlen)
{
    int result;
    struct sockaddr sa = {0};
    socklen_t len = sizeof(sa);

    // Pass through the request.
    result = sym_next(getsockopt, sockfd, level, optname, optval, optlen);

    // If it worked, we're good.
    if (result != -1 || wslcompat_passthru_self())
        return result;

    // Test if it's an error we handle.
    if (errno != EINVAL && errno != ENOPROTOOPT)
        return result;

    // Translate SO_REUSEPORT (Windows allows port sharing via SO_REUSEADDR).
    if (errno == ENOPROTOOPT && level == SOL_SOCKET && optname == SO_REUSEPORT) {
        wsldbg("fd=%d: reading SO_REUSEPORT via SO_REUSEADDR", sockfd);
        return sym_next(getsockopt, sockfd, level, SO_REUSEADDR, optval, optlen);
    }

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
        memset(optval, 0, *optlen);
        memcpy(optval, &sa.sa_family, sizeof sa.sa_family);
        *optlen = sizeof sa.sa_family;
        return 0;
    }

    // This one is always 0 for AF_UNIX
    if (optname == SO_PROTOCOL && *optlen >= sizeof(int)) {
        memset(optval, 0, sizeof(int));
        *optlen = sizeof(int);
        return 0;
    }

    return result;
}
