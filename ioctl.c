#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#define termios asmtermios
#include <asm/termbits.h>
#undef termios
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include "tunables.h"
#include "logging.h"
#include "shim.h"
#include "synth.h"

int wslcompat_tty_install(int fd, const struct termios *initial);

SHIM_INIT(ioctl, isatty, tcgetattr, tcsetattr, tcsetpgrp, tcgetpgrp, tcdrain,
          tcsendbreak, tcflush, tcflow, tcgetsid, ttyname, ttyname_r);

// This wrapper passes all calls straight through to the kernel. If it sees
// EBADF, that is a signal this might be a synthetic file descriptor that
// we generated to cook terminal i/o.
//
// If a user calls TCSETS, we take over the fd and start cooking i/o to handle
// termios settings wsl1 doesn't implement.
int ioctl(int fd, unsigned long op, ...)
{
    va_list ap;
    void *arg;
    int result;

    va_start(ap, op);
    arg = va_arg(ap, void *);
    va_end(ap);

    result = sym_next(ioctl, fd, op, arg);

    if (wslcompat_passthru_self())
        return result;

    wsltrace("fd=%d, op=%lu arg=%p", fd, op, arg);

    // EBADF means the fd could have been a wrapped synthetic fd.
    if (result == -1 && errno == EBADF) {
        SYNTH_AUTOPIN_FD(e, fd);

        // We don't recognize this fd, it's not one of ours.
        if (e == NULL) {
            wsldbg("fd=%d not recognized", fd);
            return result;
        }

        switch (op) {
            // The FD_CLOEXEC flag is the only per-fd flag that doesn't apply
            // to the OFD. It can only be set by open/fcntl/ioctl.
            case FIOCLEX:
                result = syscall(SYS_fcntl, fd, F_SETFD, FD_CLOEXEC);
                break;
            case FIONCLEX:
                result = syscall(SYS_fcntl, fd, F_SETFD, 0);
                break;
            default:
                SYNTH_DISPATCH(ioctl, fd, op, arg);
                return result;
        }

        return result;
    }

    if (result == 0) {
        switch (op) {
            case TCSETS:
            case TCSETSW:
            case TCSETSF:
            case TCSETS2:
            case TCSETSW2:
            case TCSETSF2:
                // This is a terminal, user wants to change terminal options -
                // we need to cook it.
                wsldebug("a tty ioctl %lx succeeded on fd %d, wrapping", op, fd);
                wslcompat_tty_install(fd, arg);
                break;
        }
    }

    return result;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    unsigned long int cmd;

    if (wslcompat_passthru_self())
        return sym_next(tcsetattr, fd, optional_actions, termios_p);

    wsltrace("fd=%d, optional_actions=%d, termios_p=%p", fd, optional_actions, termios_p);

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
            wsldbg("unrecognized optional_actions %d", optional_actions);
            return errno = EINVAL, -1;
    }

    return ioctl(fd, cmd, termios_p);
}

// glibc's tcgetattr inlines raw TCGETS, bypassing our ioctl shim --
// route through ioctl() so wrapped fds dispatch via synth.
int tcgetattr(int fd, struct termios *termios_p)
{
    if (wslcompat_passthru_self())
        return sym_next(tcgetattr, fd, termios_p);

    wsltrace("fd=%d, termios_p=%p", fd, termios_p);

    return ioctl(fd, TCGETS, termios_p);
}

// Same reason as tcgetattr: glibc bypasses our ioctl shim, so
// wrapped fds need explicit routing.
int isatty(int fd)
{
    struct termios t;

    if (wslcompat_passthru_self())
        return sym_next(isatty, fd);

    wsltrace("fd=%d", fd);

    return ioctl(fd, TCGETS, &t) == 0;
}

// The tc* job-control wrappers below all internally call __ioctl
// (glibc's direct-bound symbol), which bypasses our libc ioctl
// interposer.
int tcsetpgrp(int fd, pid_t pgrp)
{
    if (wslcompat_passthru_self())
        return sym_next(tcsetpgrp, fd, pgrp);

    wsltrace("fd=%d pgrp=%d", fd, pgrp);

    return ioctl(fd, TIOCSPGRP, &pgrp);
}

pid_t tcgetpgrp(int fd)
{
    pid_t pgrp;

    if (wslcompat_passthru_self())
        return sym_next(tcgetpgrp, fd);

    wsltrace("fd=%d", fd);

    if (ioctl(fd, TIOCGPGRP, &pgrp) < 0)
        return -1;
    return pgrp;
}

pid_t tcgetsid(int fd)
{
    pid_t sid;

    if (wslcompat_passthru_self())
        return sym_next(tcgetsid, fd);

    wsltrace("fd=%d", fd);

    if (ioctl(fd, TIOCGSID, &sid) < 0)
        return -1;
    return sid;
}

int tcdrain(int fd)
{
    if (wslcompat_passthru_self())
        return sym_next(tcdrain, fd);

    wsltrace("fd=%d", fd);

    return ioctl(fd, TCSBRK, (void *)(intptr_t)1);
}

int tcsendbreak(int fd, int duration)
{
    if (wslcompat_passthru_self())
        return sym_next(tcsendbreak, fd, duration);

    wsltrace("fd=%d duration=%d", fd, duration);

    // POSIX leaves nonzero duration implementation-defined; match
    // glibc's behavior and send a zero-length break.
    return ioctl(fd, TCSBRK, (void *)(intptr_t)0);
}

int tcflush(int fd, int queue_selector)
{
    if (wslcompat_passthru_self())
        return sym_next(tcflush, fd, queue_selector);

    wsltrace("fd=%d queue=%d", fd, queue_selector);

    return ioctl(fd, TCFLSH, (void *)(intptr_t)queue_selector);
}

int tcflow(int fd, int action)
{
    if (wslcompat_passthru_self())
        return sym_next(tcflow, fd, action);

    wsltrace("fd=%d action=%d", fd, action);

    return ioctl(fd, TCXONC, (void *)(intptr_t)action);
}

char *ttyname(int fd)
{
    int real;

    if (wslcompat_passthru_self())
        return sym_next(ttyname, fd);

    wsltrace("fd=%d", fd);

    if ((real = wslcompat_synth_passthru(fd, SYNTH_OP_IO)) >= 0)
        return sym_next(ttyname, real);

    return sym_next(ttyname, fd);
}

int ttyname_r(int fd, char *buf, size_t buflen)
{
    int real;

    if (wslcompat_passthru_self())
        return sym_next(ttyname_r, fd, buf, buflen);

    wsltrace("fd=%d, buf=%p, buflen=%zu", fd, buf, buflen);

    if ((real = wslcompat_synth_passthru(fd, SYNTH_OP_IO)) >= 0)
        return sym_next(ttyname_r, real, buf, buflen);

    return sym_next(ttyname_r, fd, buf, buflen);
}
