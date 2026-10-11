#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(syscall);

#define SYSCALL_REDIRECT(fn) do {                                    \
    case SYS_##fn:                                                   \
        if (wslcompat_tunable_list("syscall", #fn) && !redir) {      \
            extern long __wslcompat_##fn(long, ...);                 \
            redir++;                                                 \
            long _r = __wslcompat_##fn(a0,a1,a2,a3,a4,a5);           \
            redir--;                                                 \
            return _r;                                               \
        }                                                            \
        break;                                                       \
    } while (0)

// Callers that drop to syscall() have explicitly opted out of interception.
// The user can override this with the user.wslcompat.syscall tunable.
long syscall(long number, ...)
{
    static _Thread_local int redir;
    va_list ap;
    va_start(ap, number);

    long a0 = va_arg(ap, long);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);

    va_end(ap);

    switch (number) {
        SYSCALL_REDIRECT(execve);
        SYSCALL_REDIRECT(mmap);
    }

    // execve is not intercepted by default, but does require it, which may cause confusing bugs.
    // log this here so we get a clue something odd is happening.
    if (number == SYS_execve) {
        wsltrace("execve(%#lx) called via syscall()", a0);
    }

    return sym_next(syscall, number, a0, a1, a2, a3, a4, a5);
}
