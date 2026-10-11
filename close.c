#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "internal.h"
#include "logging.h"
#include "shim.h"
#include "synth.h"
#include "utils.h"

SHIM_INIT(close, dup2, dup3, close_range);

// A problem we have is that we need our own book-keeping file descriptors, and
// a program might try to dup() onto it, or closes all open file descriptors,
// or maybe a shell script does exec 4<foo.
//
// To avoid this problem we intercept those operations and check if we need to
// relocate our descriptors out of the way.
static void relocate_internal(int targetfd)
{
    wslcompat_synth_relocate_internal(targetfd);
    wslcompat_relocate_internal_fd(targetfd);
}

int close(int fd)
{
    struct synth_entry *e;
    int r;

    if (wslcompat_passthru_self())
        return sym_next(close, fd);

    wsltrace("fd=%d", fd);

    e = wslcompat_synth_pre_close(fd);

    if (e && e->ops->close)
        e->ops->close(e);

    relocate_internal(fd);

    r = sym_next(close, fd);

    preserve_errno();

    wslcompat_synth_post_close(e);

    return r;
}

// dup2/dup3 silently close newfd if it was open before duplicating
// oldfd onto it. That implicit close bypasses our close() shim, so
// we run the same pre_close/post_close dance manually around the
// syscall.
static int dup_common(int oldfd, int newfd, int flags, bool allow_self)
{
    struct synth_entry *e;
    int r;

    wsltrace("oldfd=%d newfd=%d flags=%d", oldfd, newfd, flags);

    if (allow_self && oldfd == newfd)
        return sym_next(dup2, oldfd, newfd);

    // Move any of our internal fds out of the way (probe_fd, real_fd, debugfd, ...).
    relocate_internal(newfd);

    // Check if this is one of our synthetic file descriptors (exposed to
    // calller) closing.
    e = wslcompat_synth_pre_close(newfd);

    if (e && e->ops->dup)
        e->ops->dup(e);

    r = sym_next(dup3, oldfd, newfd, flags);

    preserve_errno();

    wslcompat_synth_post_close(e);

    return r;
}

int dup2(int oldfd, int newfd)
{
    if (wslcompat_passthru_self())
        return sym_next(dup2, oldfd, newfd);
    return dup_common(oldfd, newfd, 0, true);
}

int dup3(int oldfd, int newfd, int flags)
{
    if (wslcompat_passthru_self())
        return sym_next(dup3, oldfd, newfd, flags);
    return dup_common(oldfd, newfd, flags, false);
}

struct close_range_args {
    unsigned int first;
    unsigned int last;
};

static int close_range_visit(int fd, void *user)
{
    struct close_range_args *args = user;
    if ((unsigned int)fd >= args->first && (unsigned int)fd <= args->last) {
        close(fd);
    }
    return 0;
}

int close_range(unsigned int first, unsigned int last, int flags)
{
    struct close_range_args args = { first, last };

    if (wslcompat_passthru_self())
        return sym_next(close_range, first, last, flags);

    if (first > last || flags != 0)
        return errno = EINVAL, -1;

    wslcompat_walk_proc_self_fd(close_range_visit, &args);
    return 0;
}
