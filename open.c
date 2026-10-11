#define _GNU_SOURCE
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <sys/syscall.h>

#include "shim.h"
#include "synth.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(openat, open);

// We can redirect opens to missing files (e.g. /proc entries), this is the
// callback prototype for that.
typedef int (*open_handler_fn)(int dirfd, const char *path, int flags, mode_t mode, int *fd);

// WSL has no concept of btimes, so we store our own in this extended attribute.
#define BTIME_XATTR "user.wslcompat.btime"

// File path handlers
#include "proc.c"

// Hardcoded dispatch table for paths we polyfill. The pattern field is
// an fnmatch(3) pattern.
static const struct {
    const char *pattern;
    open_handler_fn fn;
} dispatch[] = {
    { "/proc/sys/vm/mmap_min_addr", handle_mmap_min_addr },
    { "/proc/*/mem",                handle_proc_pid_mem  },
    { "/proc/*/task/*/mem",         handle_proc_pid_mem  },
};

static void record_btime_now(int fd);

// WSL1 doesn't recognize O_TMPFILE, so we create a randomly-named file, then unlink.
static int handle_tmpfile_at(int dirfd, const char *path, int flags, mode_t mode)
{
    unsigned long long r;
    char name[64];
    int realdir = -1;
    int fd = -1;

    int oflags = (flags & ~O_TMPFILE) | O_CREAT | O_EXCL;

    if ((realdir = sym_next(openat, dirfd, path, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0) {
        wsldbg("failed to open dirfd for O_TMPFILE");
        goto cleanup;
    }

    if (syscall(SYS_getrandom, &r, sizeof(r), 0) != sizeof(r)) {
        wslwarn("failed to generate a random name for O_TMPFILE");
        goto cleanup;
    }

    snprintf(name, sizeof(name), ".wslcompat.%016llx", r);

    if ((fd = sym_next(openat, realdir, name, oflags, mode)) >= 0) {
        wsldbg("O_TMPFILE polyfill: tmp=%s dirfd=%d fd=%d", name, dirfd, fd);
        record_btime_now(fd);
        syscall(SYS_unlinkat, realdir, name, 0);
    }

cleanup:
    if (realdir != -1)
        syscall(SYS_close, realdir);
    return fd;
}

// Record the current time as the birth time on a fd we just created.
// This is read back by the STATX_BTIME polyfill.
static void record_btime_now(int fd)
{
    struct timespec ts;
    char buf[32];

    if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts) != 0)
        return;

    int n = snprintf(buf, sizeof(buf), "%ld.%09ld", ts.tv_sec, ts.tv_nsec);

    if (syscall(SYS_fsetxattr, fd, BTIME_XATTR, buf, n, XATTR_CREATE) != 0)
        wsldbg("fd=%d: fsetxattr(btime) failed: %m", fd);
}

// open() is just openat(AT_FDCWD, ...) in glibc -- forward everything through
// so the logic lives in just one place.
int open(const char *pathname, int flags, ...)
{
    va_list ap;
    mode_t mode = 0;

    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (wslcompat_passthru_self())
        return sym_next(open, pathname, flags, mode);

    return openat(AT_FDCWD, pathname, flags, mode);
}

// Walk the dispatch table looking for a handler that claims pathname.
static int match_redirects_table(int dirfd, const char *pathname, int flags,
                                 mode_t mode, int *out_fd)
{
    if (wslcompat_tunable_bool("redirect", true) == false)
        return 0;

    for (size_t i = 0; i < _countof(dispatch); i++) {
        if (fnmatch(dispatch[i].pattern, pathname, FNM_PATHNAME) != 0)
            continue;

        wsldbg("%s matches %s, calling handler", pathname, dispatch[i].pattern);

        // Pattern matched -- offer it to the handler, which may still
        // decline (return 0) to let dispatch continue.
        if (dispatch[i].fn(dirfd, pathname, flags, mode, out_fd) != 0) {
            wsldbg("handler accepted %s, fd=%d", pathname, *out_fd);
            return 1;
        }
    }
    return 0;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    int fd;
    va_list ap;
    mode_t mode = 0;

    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (wslcompat_passthru_self())
        return sym_next(openat, dirfd, pathname, flags, mode);

    if (((flags & O_TMPFILE) == O_TMPFILE) && !(flags & O_PATH))
        return handle_tmpfile_at(dirfd, pathname, flags, mode);

    if (!pathname)
        return sym_next(openat, dirfd, pathname, flags, mode);

    if (match_redirects_table(dirfd, pathname, flags, mode, &fd))
        return fd;

    // Pass-through if we're not creating a file, or if O_PATH is set (which ignores O_CREAT).
    if (!(flags & O_CREAT) || (flags & O_PATH))
        return sym_next(openat, dirfd, pathname, flags, mode);

    // We *are* creating a file, we add O_EXCL so we can record btime.
    if ((fd = sym_next(openat, dirfd, pathname, flags | O_EXCL, mode)) >= 0) {
        record_btime_now(fd);
        return fd;
    }

    // If O_EXCL fails, we can't record any btime and statx will fall back to a
    // heuristic. If the user requested O_EXCL, we're done.
    if (errno != EEXIST || (flags & O_EXCL))
        return -1;

    // The user didn't request O_EXCL, so try again with plain O_CREAT.
    return sym_next(openat, dirfd, pathname, flags, mode);
}

// FORTIFY_SOURCE rewrites open(path, flags) (no mode arg) into
// __open_2(path, flags) when flags isn't a compile-time constant.
int __open_2(const char *pathname, int flags)
{
    wsltrace("pathname=%s flags=%d", pathname, flags);
    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE))
        wslcompat_chk_fail();
    return open(pathname, flags);
}

int __openat_2(int dirfd, const char *pathname, int flags)
{
    wsltrace("dirfd=%d pathname=%s flags=%d", dirfd, pathname, flags);
    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE))
        wslcompat_chk_fail();
    return openat(dirfd, pathname, flags);
}

int creat(const char *pathname, mode_t mode)
{
    return open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

// On glibc the *64 variants are the same entry points as the non-64 versions,
// but the symbols are exported separately so we need to alias them.
int __open64_2(const char *pathname, int flags)
    __attribute__((alias("__open_2")));
int __openat64_2(int dirfd, const char *pathname, int flags)
    __attribute__((alias("__openat_2")));
int open64(const char *pathname, int flags, ...)
    __attribute__((alias("open")));
int openat64(int dirfd, const char *pathname, int flags, ...)
    __attribute__((alias("openat")));
int creat64(const char *pathname, mode_t mode)
    __attribute__((alias("creat")));
