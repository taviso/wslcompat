/* FIOCLEX/FIONCLEX on a synth wrap must affect the caller's fd, not
 * be forwarded to the hidden backing real_fd via SYNTH_DISPATCH.
 *
 * The bug: our ioctl shim caught the kernel's EBADF on the O_PATH
 * wrap and dispatched via tty_ioctl, which forwards everything to
 * real_fd. CLOEXEC is per-fd not per-OFD, so clearing/setting it on
 * the backing left the user's wrap fd unchanged. Python's
 * os.set_inheritable() uses ioctl(FIONCLEX) as its atomic primitive,
 * so set_inheritable(wrap, True) silently failed -- the wrap kept
 * CLOEXEC=1, portage's _setup_pipes propagated that to fd 1/2 via
 * fcntl, preexec_save preserved it across dup3, and kernel execve
 * closed fd 1/2 on the build worker. Net symptom: no build output.
 *
 * Wrap a pty slave via tcsetattr, flip CLOEXEC via the ioctl path,
 * observe via fcntl(F_GETFD).
 *
 * Native Linux: no wrap; kernel handles FIOCLEX natively. Shimmed:
 * special case in ioctl.c translates to fcntl(F_SETFD) on the user
 * fd. Either way, XPASS. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0) err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL) err(EXIT_FAILURE, "ptsname");

    int slave = open(sname, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) err(EXIT_FAILURE, "open slave");

    // tcsetattr triggers tty_install under the shim; no-op natively.
    struct termios attrs;
    if (tcgetattr(slave, &attrs) != 0) err(EXIT_FAILURE, "tcgetattr");
    if (tcsetattr(slave, TCSANOW, &attrs) != 0) err(EXIT_FAILURE, "tcsetattr");

    // Show whether we're testing the wrap path or the bare-kernel path.
    long rawflags = syscall(SYS_fcntl, slave, F_GETFL);
    printf("slave fd=%d raw F_GETFL=0x%lx%s\n", slave, rawflags,
           (rawflags >= 0 && (rawflags & O_PATH)) ? " [wrap]" : " [real fd]");

    int fail = 0;

    // Opened with O_CLOEXEC, so CLOEXEC should be set going in.
    int flags = fcntl(slave, F_GETFD);
    if (flags != FD_CLOEXEC) {
        printf("FAIL: initial F_GETFD=0x%x (expected 0x%x)\n",
               flags, FD_CLOEXEC);
        fail++;
    }

    // FIONCLEX: clear CLOEXEC. Before fix this silently no-op'd on
    // the wrap because the shim forwarded to backing real_fd.
    if (ioctl(slave, FIONCLEX) != 0) {
        printf("FAIL: ioctl(FIONCLEX): %m\n");
        fail++;
    }
    flags = fcntl(slave, F_GETFD);
    if (flags != 0) {
        printf("FAIL: after FIONCLEX F_GETFD=0x%x (expected 0)\n", flags);
        fail++;
    }

    // FIOCLEX: set CLOEXEC. Same shape, opposite direction.
    if (ioctl(slave, FIOCLEX) != 0) {
        printf("FAIL: ioctl(FIOCLEX): %m\n");
        fail++;
    }
    flags = fcntl(slave, F_GETFD);
    if (flags != FD_CLOEXEC) {
        printf("FAIL: after FIOCLEX F_GETFD=0x%x (expected 0x%x)\n",
               flags, FD_CLOEXEC);
        fail++;
    }

    close(slave);
    close(master);

    if (fail) {
        printf("FAIL: %d check(s) failed\n", fail);
        return 1;
    }
    printf("PASS: FIOCLEX/FIONCLEX affect wrap fd CLOEXEC\n");
    return 0;
}
