#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(execveat);

int execveat(int dirfd, const char *pathname,
             char *const argv[], char *const envp[], int flags)
{
    char        buf[PATH_MAX];
    int         len;

    HIDE_NONNULL(pathname);

    if (wslcompat_passthru_self())
        return sym_next(execveat, dirfd, pathname, argv, envp, flags);

    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
        return errno = EINVAL, -1;

    if (pathname == NULL)
        return errno = EFAULT, -1;

    if (*pathname == '\0' && !(flags & AT_EMPTY_PATH))
        return errno = ENOENT, -1;

    if (dirfd != AT_FDCWD && pathname[0] != '/') {
        if (fcntl(dirfd, F_GETFD) == -1) {
            return -1; // fcntl sets errno to EBADF
        }
    }

    if (flags & AT_SYMLINK_NOFOLLOW) {
        struct stat st;
        if (fstatat(dirfd, pathname, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISLNK(st.st_mode))
                return errno = ELOOP, -1;
        }
    }

    if (*pathname == '\0' && (flags & AT_EMPTY_PATH)) {
        // fexecve-style: dirfd is the binary itself.
        len = snprintf(buf, sizeof(buf), "/proc/self/fd/%d", dirfd);
    } else if (pathname[0] == '/') {
        len = snprintf(buf, sizeof(buf), "%s", pathname);
    } else if (dirfd == AT_FDCWD) {
        len = snprintf(buf, sizeof(buf), "%s", pathname);
    } else {
        len = snprintf(buf, sizeof(buf), "/proc/self/fd/%d/%s", dirfd, pathname);
    }

    if (len < 0 || (size_t)len >= sizeof(buf))
        return errno = ENAMETOOLONG, -1;

    return execve(buf, argv, envp);
}
