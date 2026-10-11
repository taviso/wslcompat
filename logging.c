#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/syscall.h>

#include "tunables.h"
#include "logging.h"
#include "internal.h"

static int        loglevel = -1;
static atomic_int debugfd  = -2;

static void __attribute__((constructor)) init(void)
{
    wslcompat_register_internal_fd(&debugfd);
}

static void __attribute__((destructor)) fini(void)
{
    int fd = atomic_load(&debugfd);
    if (fd >= 0)
        syscall(SYS_close, fd);
}

int wslcompat_debug_log(int level, const char *tag, const char *fmt, ...)
{
    char prefix[64];
    char buf[512];
    va_list ap;
    int fd;
    struct iovec logbuf[] = {
        { .iov_len = 0, .iov_base = prefix    },
        { .iov_len = 0, .iov_base = buf       },
        { .iov_len = 1, .iov_base = "\n"      },
    };

    // Check if logging is enabled.
    if (loglevel < 0)
        loglevel = wslcompat_tunable_int("debug", 0);

    // Check if this log message is wanted.
    if (level > loglevel)
        return 0;

    // Lazy open. Uses compare_exchange to avoid leaking an fd if
    // multiple threads try to initialize simultaneously.
    if ((fd = atomic_load(&debugfd)) < -1) {
        int new_fd = syscall(SYS_open,
                             wslcompat_tunable_str("logfile", "/dev/tty"),
                             O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        int expected = -2;
        if (atomic_compare_exchange_strong(&debugfd, &expected, new_fd)) {
            fd = new_fd;
        } else {
            // We lost the race, another thread initialized it. Close ours.
            if (new_fd >= 0) syscall(SYS_close, new_fd);
            fd = expected;
        }
    }

    // If that fails, nothing we can do.
    if (fd < 0)
        return -1;

    // Prepare log message.
    va_start(ap, fmt);
    logbuf[0].iov_len = snprintf(prefix, sizeof(prefix), "[wsl] %ld %s: ", syscall(SYS_getpid), tag);
    logbuf[1].iov_len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // If we truncated, clamp the length so we still print the truncated message
    // (the writev trick ensures our newline is still appended!)
    if (logbuf[0].iov_len >= sizeof(prefix))
        logbuf[0].iov_len = sizeof(prefix) - 1;
    if (logbuf[1].iov_len >= sizeof(buf))
        logbuf[1].iov_len = sizeof(buf) - 1;

    // Pass to writev() to assemble.
    return syscall(SYS_writev, fd, logbuf, 3);
}
