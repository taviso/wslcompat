/* Companion to epoll_fork_order_test: confirm that the
 * "do everything after fork" pattern is NOT affected by the same
 * inotify_fd ordering hazard.
 *
 * Scenario:
 *   - Parent forks. Neither parent nor child has touched inotify yet.
 *   - Child creates its OWN epfd (register_inotify lazy-opens child's
 *     inotify_fd and registers it in child's epfd).
 *   - Child creates a synth wrap; watch lands on child's inotify_fd
 *     which IS in child's epfd. Wakeup works.
 *
 * Should XPASS both unshimmed and shimmed. */
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

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Child does EVERYTHING after fork: epfd, wrap, wait.
        int ep = epoll_create1(EPOLL_CLOEXEC);
        if (ep < 0) _exit(9);

        int slave = open(sname, O_RDWR | O_NOCTTY);
        if (slave < 0) _exit(10);

        struct termios attrs;
        if (tcgetattr(slave, &attrs) != 0) _exit(11);
        attrs.c_lflag &= ~(ICANON | ECHO);
        if (tcsetattr(slave, TCSANOW, &attrs) != 0) _exit(12);

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

    close(master);
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS: child epoll_wait got POLLHUP (epfd created after fork)\n");
        return 0;
    }
    printf("FAIL: child exit status=%d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 1;
}
