/* Minimal reproducer for the emerge pty hang.
 *
 * tty_install wraps the slave by F_DUPFD'ing it into a private
 * real_fd. That dup is supposed to be reaped when the user closes
 * their wrap, via the synth.c liveness probe (flock LOCK_SH on the
 * synth O_PATH, LOCK_EX|LOCK_NB on a separate probe fd to detect
 * last-close). The probe is returning EAGAIN after the user's close
 * in the real emerge scenario, so real_fd never gets reaped, slave
 * OFD stays alive, master never sees POLLHUP, asyncio hangs.
 *
 * The probe machinery works correctly in isolation (see
 * probe_semantics_test.c) -- something about this specific flow
 * trips it up. The test is tiny on purpose so the failing strace
 * has minimal noise.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0)
        err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0)
        err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL)
        err(EXIT_FAILURE, "ptsname");

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0)
        err(EXIT_FAILURE, "open slave");

    // tcsetattr triggers tty_install: it F_DUPFD's the slave into our
    // private real_fd, then dup3's a synth O_PATH over `slave`.
    struct termios attrs;
    if (tcgetattr(slave, &attrs) != 0)
        err(EXIT_FAILURE, "tcgetattr");
    attrs.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(slave, TCSANOW, &attrs) != 0)
        err(EXIT_FAILURE, "tcsetattr");

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        dup2(slave, 1);
        _exit(0);
    }

    // Drop our visible slave reference. The child still holds slave
    // via stdout until it exits.
    close(slave);
    waitpid(pid, NULL, 0);

    // Master must now report POLLHUP -- all slave references are
    // gone (child's stdout died on exit, ours on close). Under the
    // shim, our hidden real_fd dup keeps slave alive and POLLHUP
    // never fires.
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        err(EXIT_FAILURE, "epoll_create1");

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLHUP,
        .data.fd = master,
    };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, master, &ev) != 0)
        err(EXIT_FAILURE, "epoll_ctl ADD");

    struct epoll_event got;
    int n = epoll_wait(ep, &got, 1, 2000);

    close(ep);
    close(master);

    if (n > 0) {
        printf("PASS: master events=0x%x\n", got.events);
        return 0;
    }
    printf("FAIL: master never reported events (n=%d)\n", n);
    return 1;
}
