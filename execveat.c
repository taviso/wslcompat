#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(execveat);

int execveat(int dirfd, const char *pathname,
             char *const argv[], char *const envp[], int flags)
{
    char        buf[PATH_MAX];
    const char *target;

    // Optimization barrier: hide 'pathname' from GCC's nonnull analysis
    asm volatile ("" : "+r" (pathname));

    if (wslcompat_passthru("execveat"))
        return sym_next(execveat, dirfd, pathname, argv, envp, flags);

    if (pathname == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (*pathname == '\0' && (flags & AT_EMPTY_PATH)) {
        // fexecve-style: dirfd is the binary itself.
        snprintf(buf, sizeof(buf), "/proc/self/fd/%d", dirfd);
    } else if (pathname[0] == '/') {
        snprintf(buf, sizeof(buf), "%s", pathname);
    } else if (dirfd == AT_FDCWD) {
        snprintf(buf, sizeof(buf), "%s", pathname);
    } else {
        snprintf(buf, sizeof(buf), "/proc/self/fd/%d/%s", dirfd, pathname);
    }

    return execve(buf, argv, envp);
}
