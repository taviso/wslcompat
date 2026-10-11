#define _GNU_SOURCE
#include <stddef.h>
#include <sys/socket.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(setsockopt);

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen)
{
    if (wslcompat_passthru_self())
        return sym_next(setsockopt, sockfd, level, optname, optval, optlen);

    wsltrace("sockfd=%d, level=%d, optname=%d", sockfd, level, optname);

    // WSL1 BUG: setsockopt(SO_REUSEPORT) actually succeeds natively, but silently fails
    // to provide port-sharing behavior. Therefore, we cannot use a canary check here.
    // We must unconditionally translate it to SO_REUSEADDR (which Windows uses for port sharing).
    if (level == SOL_SOCKET && optname == SO_REUSEPORT) {
        wsldbg("fd=%d: translating SO_REUSEPORT to SO_REUSEADDR", sockfd);
        optname = SO_REUSEADDR;
    }

    return sym_next(setsockopt, sockfd, level, optname, optval, optlen);
}
