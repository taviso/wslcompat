#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include "tunables.h"
#include "logging.h"

static int loglevel = -1;
static int debugfd  = -2;

static void __attribute__((destructor)) fini(void)
{
    if (debugfd >= 0)
        close(debugfd);
}

int wslcompat_debug_log(int level, const char *tag, const char *fmt, ...)
{
    char prefix[64];
    char buf[512];
    va_list ap;
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

    // Check if we have an output descriptor.
    if (debugfd < -1)
        debugfd = open("/dev/tty", O_WRONLY | O_CLOEXEC);

    // If that fails, nothing we can do.
    if (debugfd < 0)
        return -1;

    // Prepare log message.
    va_start(ap, fmt);
    logbuf[0].iov_len = snprintf(prefix, sizeof(prefix), "[wsl] %s: ", tag);
    logbuf[1].iov_len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Check that didn't truncate.
    if (logbuf[0].iov_len >= sizeof(prefix))
        return -1;
    if (logbuf[1].iov_len >= sizeof(buf))
        return -1;

    // Pass to writev() to assemble.
    return writev(debugfd, logbuf, 3);
}
