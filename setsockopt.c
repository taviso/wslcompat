#define _GNU_SOURCE
#include <stddef.h>
#include <sys/socket.h>

#include "shim.h"
#include "tunables.h"

SHIM_INIT(setsockopt);

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen)
{
    if (wslcompat_passthru("setsockopt"))
        return sym_next(setsockopt, sockfd, level, optname, optval, optlen);

    // Translate SO_REUSEPORT to SO_REUSEADDR (Windows semantics allow port
    // sharing via SO_REUSEADDR).
    if (level == SOL_SOCKET && optname == SO_REUSEPORT)
        optname = SO_REUSEADDR;

    return sym_next(setsockopt, sockfd, level, optname, optval, optlen);
}
