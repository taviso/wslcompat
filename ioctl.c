#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <stdint.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/param.h>

#define MAX_FDS 8192

static int (*sym_ioctl)(int fd, unsigned long op, ...);
static ssize_t (*sym_read)(int fd, void *buf, size_t count);

static uint8_t fd_seen[howmany(MAX_FDS, NBBY)];
static uint8_t fd_intercept[howmany(MAX_FDS, NBBY)];
static bool read_intercept;

static void __attribute__((constructor)) init(void)
{
    sym_ioctl = dlsym(RTLD_NEXT, "ioctl");
    sym_read  = dlsym(RTLD_NEXT, "read");
    return;
}

static void handle_tcset(int fd, struct termios *tio)
{
    // Verify this is in range.
    if (fd < 0 || fd >= MAX_FDS)
        return;

    // Record that we know this filedescriptor.
    setbit(fd_seen, fd);

    // Check if caller is trying to disable canonical mode.
    if (tio->c_lflag & ICANON) {
        // Canonical mode disabled, make sure we're not hooking this.
        clrbit(fd_intercept, fd);
        return;
    }

    // Canonical mode enabled, we need to cook any reads.
    setbit(fd_intercept, fd);

    // Make sure read interception is now globally enabled.
    read_intercept = true;

    return;
}

int ioctl(int fd, unsigned long op, ...)
{
    va_list ap;
    void *arg;
    int result;

    va_start(ap, op);
    arg = va_arg(ap, void *);
    va_end(ap);

    result = sym_ioctl(fd, op, arg);

    if (result == 0) {
        switch (op) {
            case TCSETSW:
            case TCSETSF:
            case TCSETS:
            case TCSETSW2:
            case TCSETSF2:
            case TCSETS2:
                handle_tcset(fd, arg);
                break;
        }
    }

    return result;
}

ssize_t read(int fd, void *buf, size_t count) {
    struct termios t = {0};
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    cc_t *vmin  = &t.c_cc[VMIN];
    cc_t *vtime = &t.c_cc[VTIME];
    char *ptr = buf;

    // Verify interception is globally enabled.
    if (read_intercept == false)
        goto passthru;

    // Check if this is an fd we can track.
    if (fd < 0 || fd >= MAX_FDS)
        goto passthru;

    // Check if this is a file descriptor we might be monitoring.
    if (isset(fd_seen, fd) && isclr(fd_intercept, fd))
        goto passthru;

    // It either isn't seen, or is being monitored.
    if (isclr(fd_seen, fd) || isset(fd_intercept, fd)) {
        // We now know this fd exists.
        setbit(fd_seen, fd);

        // Check if this is a tty
        if (sym_ioctl(fd, TCGETS, &t) != 0) {
            // Not a tty, or not anymore, so we can clear it.
            clrbit(fd_intercept, fd);
            goto passthru;
        }

        // It is a terminal, so we do need to monitor it.
        setbit(fd_intercept, fd);
    }

    // Check for canonical mode
    if (t.c_lflag & ICANON) {
        clrbit(fd_intercept, fd);
        goto passthru;
    }

    // Handle VMIN/VTIME modifications. WSL maintains these, but ignores them.
    if (*vmin == 0 && *vtime > 0) {
        int result = poll(&pfd, 1, *vtime * 100);
        if (result <= 0)
            return result;
    } else if (*vmin > 0 && count > 0) {
        int timeout = *vtime ? (*vtime * 100) : -1;
        int result = poll(&pfd, 1, -1);
        size_t total = 0;
        ssize_t n;

        if (result <= 0)
            return result;

        if ((n = sym_read(fd, ptr, count)) <= 0)
            return n;

        total += n;
        ptr   += n;

        while (total < *vmin && total < count) {
            if (poll(&pfd, 1, timeout) <= 0)
                break;
            if ((n = sym_read(fd, ptr, count - total)) <= 0)
                break;
            total += n;
            ptr   += n;
        }
        return total;
    } else if (*vmin == 0 && *vtime == 0) {
        int result = poll(&pfd, 1, 0);

        // Check for errors or empty buffer
        if (result <= 0)
            return result;

        // Data is waiting, so the underlying read will not block
        goto passthru;
    }

  passthru:
    return sym_read(fd, buf, count);
}

int tcsetattr(int fd, int optional_actions, struct termios *termios_p)
{
    unsigned long int cmd;

    switch (optional_actions) {
        case TCSANOW:
            cmd = TCSETS;
            break;
        case TCSADRAIN:
            cmd = TCSETSW;
            break;
        case TCSAFLUSH:
            cmd = TCSETSF;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    return ioctl(fd, cmd, termios_p);
}
