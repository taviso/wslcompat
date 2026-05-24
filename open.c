#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/random.h>
#include <sys/xattr.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(openat, open);

#define BTIME_XATTR "user.wslcompat.btime"

typedef int (*open_handler_fn)(int dirfd, const char *pathname, int flags,
                               mode_t mode, int *out_fd);

// File path handlers
#include "proc.c"

// Hardcoded dispatch table for paths we polyfill. The pattern field is
// an fnmatch(3) pattern with FNM_PATHNAME semantics.
static const struct {
    const char *pattern;
    open_handler_fn fn;
} dispatch[] = {
    { "/proc/sys/vm/mmap_min_addr", handle_mmap_min_addr },
};

// WSL1 doesn't recognize O_TMPFILE, create a randomly-named file, then unlink.
static int handle_tmpfile_at(int dirfd, const char *path, int flags, mode_t mode)
{
    unsigned long long r;
    char name[64];
    int realdir = -1;
    int fd = -1;

    int oflags = (flags & ~O_TMPFILE) | O_CREAT | O_EXCL;

    if ((realdir = sym_next(openat, dirfd, path, O_PATH | O_DIRECTORY)) < 0) {
        wsldbg("failed to open dirfd for O_TMPFILE");
        goto cleanup;
    }

    if (getrandom(&r, sizeof(r), 0) != sizeof(r)) {
        wslwarn("failed to generate a random name for O_TMPFILE");
        goto cleanup;
    }

    snprintf(name, sizeof(name), ".wslcompat.%016llx", r);

    if ((fd = sym_next(openat, realdir, name, oflags, mode)) >= 0) {
        wsldbg("O_TMPFILE polyfill: tmp=%s dirfd=%d fd=%d", name, dirfd, fd);
        unlinkat(realdir, name, 0);
    }

cleanup:
    if (realdir != -1)
        close(realdir);
    return fd;
}

// Record the current time as the birth time on a fd we just created. Read back
// by the statx STATX_BTIME polyfill.
static void record_btime_now(int fd)
{
    struct timespec ts;
    char buf[32];

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;

    int n = snprintf(buf, sizeof(buf), "%ld.%09ld", ts.tv_sec, ts.tv_nsec);

    if (fsetxattr(fd, BTIME_XATTR, buf, n, XATTR_CREATE) != 0)
        wsldbg("fd=%d: fsetxattr(btime) failed: %m", fd);
}

// open() is just openat(AT_FDCWD, ...) in glibc -- forward everything through
// so the polyfill logic lives in one place.
int open(const char *pathname, int flags, ...)
{
    va_list ap;
    mode_t mode = 0;

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (wslcompat_passthru_self())
        return sym_next(open, pathname, flags, mode);

    return openat(AT_FDCWD, pathname, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    int fd;
    va_list ap;
    mode_t mode = 0;

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (wslcompat_passthru_self())
        return sym_next(openat, dirfd, pathname, flags, mode);

    if ((flags & O_TMPFILE) == O_TMPFILE)
        return handle_tmpfile_at(dirfd, pathname, flags, mode);

    if (!pathname)
        return sym_next(openat, dirfd, pathname, flags, mode);

    for (size_t i = 0; i < _countof(dispatch); i++) {
        // Allow this feature to be disabled at runtime.
        if (wslcompat_tunable_bool("redirect", true) == false)
            break;

        // Check if we intercept this pattern.
        if (fnmatch(dispatch[i].pattern, pathname, FNM_PATHNAME) != 0)
            continue;

        // Pattern matched -- offer it to the handler, which may still
        // decline (return 0) to let dispatch continue.
        if (dispatch[i].fn(dirfd, pathname, flags, mode, &fd) != 0) {
            wsldbg("dispatched %s to handler, fd=%d", pathname, fd);
            return fd;
        }
    }

    // Pass-through if we're not creating a file.
    if (!(flags & O_CREAT))
        return sym_next(openat, dirfd, pathname, flags, mode);

    // Force O_EXCL so that we can record a guaranteed-correct btime.
    if ((fd = sym_next(openat, dirfd, pathname, flags | O_EXCL, mode)) >= 0) {
        record_btime_now(fd);
        return fd;
    }

    // If O_EXCL fails, we can't record any btime and statx will fall back to a
    // heuristic. If the user wanted O_EXCL, we're done.
    if (errno != EEXIST || (flags & O_EXCL))
        return -1;

    // The user still needs their fd so try again with plain O_CREAT.
    return sym_next(openat, dirfd, pathname, flags, mode);
}
