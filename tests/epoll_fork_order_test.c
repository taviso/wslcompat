/* Sanity check: does the inotify-in-epfd async re-probe survive
 * fork-then-create-synth ordering?
 *
 * Scenario:
 *   - Parent creates an epfd. register_inotify runs, eagerly opening
 *     the global inotify_fd and registering it in epfd as inotify
 *     FD #X.
 *   - Parent forks. Child inherits epfd (with FD #X registered) AND
 *     the inherited inotify_fd #X.
 *   - Child's first synth-relevant call hits synth_inotify_fd(),
 *     which detects pid mismatch, closes the inherited #X, and opens
 *     a fresh inotify FD #Y for itself. Watches go to #Y.
 *   - Child creates a synth wrap (pty), grandchild inherits and
 *     exits dropping the slave OFD ref. The IN_CLOSE_NOWRITE event
 *     fires on child's #Y, NOT on the stale registration #X in epfd.
 *   - Child blocks in epoll_wait expecting POLLHUP on master. With
 *     the current code, the wakeup never arrives -- child's epfd
 *     doesn't know about #Y.
 *
 * If this XPASSes, the WSL1 semantics (parent's #X stays alive
 * because parent still holds it; OFD-readable signal somehow reaches
 * the child's epoll registration anyway) save us. If it FAILs, the
 * fork gap is real and we need a fix in register_inotify (e.g.,
 * re-attempt the ADD on every epoll_wait, tolerating EEXIST).
 *
 * Either outcome is worth a permanent test: regression for the good
 * case, XFIXME tripwire for the bad case. */
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
    alarm(10);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0) err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL) err(EXIT_FAILURE, "ptsname");

    // Parent eagerly opens its inotify_fd here via register_inotify.
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) err(EXIT_FAILURE, "epoll_create1");

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Child has the parent's inotify_fd in epfd. Now create a
        // synth wrap -- watch lands on the child's FRESH inotify_fd.
        int slave = open(sname, O_RDWR | O_NOCTTY);
        if (slave < 0) _exit(10);

        struct termios attrs;
        if (tcgetattr(slave, &attrs) != 0) _exit(11);
        attrs.c_lflag &= ~(ICANON | ECHO);
        if (tcsetattr(slave, TCSANOW, &attrs) != 0) _exit(12);

        // Grandchild inherits, dups slave over stdout, exits. Slave
        // OFD destruction event fires on the child's NEW inotify_fd.
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(13);
        if (pid2 == 0) {
            dup2(slave, 1);
            _exit(0);
        }
        close(slave);
        waitpid(pid2, NULL, 0);

        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLHUP,
            .data.fd = master,
        };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, master, &ev) != 0) _exit(14);

        struct epoll_event got;
        int n = epoll_wait(ep, &got, 1, 2000);
        if (n > 0) {
            fprintf(stderr, "child: events=0x%x\n", got.events);
            _exit(0);
        }
        _exit(1);
    }

    close(ep);
    close(master);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS: child epoll_wait got POLLHUP after fork+wrap\n");
        return 0;
    }
    printf("FAIL: child exit status=%d (no async wakeup from inotify)\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 1;
}
