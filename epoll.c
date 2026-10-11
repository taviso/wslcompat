#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <time.h>

#include "logging.h"
#include "shim.h"
#include "synth.h"
#include "tunables.h"
#include "utils.h"

SHIM_INIT(epoll_ctl, epoll_create, epoll_create1, epoll_wait, epoll_pwait, epoll_pwait2);


// We need to handle epoll operations on synthetic file descriptors.
// We use a neat trick: We pass through the request unmodified and see if the
// kernel complains about the file descriptor.
//
// If there were no synthetic fds, the kernel accepts it and we do nothing -
// adding zero overhead.
//
// If it *was* a synthetic fd (i.e. an O_PATH placeholder), the kernel signals
// error on ADD with EPERM, and ENOENT on MOD/DEL. We catch those unusual
// errors and check if it we need to pass it to the synthetic callbacks.
//
// This is neat because if the caller wasn't using synthetic fds, there
// is zero overhead - we only do anything if we see an unusual errno, in which
// case we add a small amount of overhead to support new fd-types.

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    int expected;
    int real;
    int r;

    wsltrace("epfd=%d, op=%d, fd=%d, event=%p", epfd, op, fd, event);

    if ((r = sym_next(epoll_ctl, epfd, op, fd, event)) == 0)
        return r;

    if (wslcompat_passthru_self())
        return r;

    switch (op) {
        case EPOLL_CTL_ADD:
            expected = EPERM;
            break;
        case EPOLL_CTL_MOD:
        case EPOLL_CTL_DEL:
            expected = ENOENT;
            break;
        default:
            wsldbg("we dont handle op %d", op);
            return r;
    }

    if (errno != expected) {
        wsldbg("errno %d (%m), but a synth fd would return %d -> ignore", errno, expected);
        return r;
    }

    {
        preserve_errno();
        real = wslcompat_synth_passthru(fd, SYNTH_OP_POLL);
    }

    if (real < 0) {
        wsldbg("no passthru fd for %d, return %d", fd, r);
        return r;
    }

    wsltrace("synth fd=%d -> passthru fd=%d", fd, real);

    return sym_next(epoll_ctl, epfd, op, real, event);
}

int epoll_create(int size)
{
    return sym_next(epoll_create, size);
}

int epoll_create1(int flags)
{
    return sym_next(epoll_create1, flags);
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
               int timeout)
{
    return sym_next(epoll_wait, epfd, events, maxevents, timeout);
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                int timeout, const sigset_t *sigmask)
{
    return sym_next(epoll_pwait, epfd, events, maxevents, timeout, sigmask);
}

int epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                 const struct timespec *timeout, const sigset_t *sigmask)
{
    int timeout_ms = -1;
    int r = sym_next(epoll_pwait2, epfd, events, maxevents, timeout, sigmask);

    if (r != -1 || errno != ENOSYS || wslcompat_passthru_self())
        return r;

    wsltrace("epfd=%d", epfd);

    if (timeout) {
        timeout_ms = timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
    }

    return epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask);
}
