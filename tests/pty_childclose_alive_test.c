/* Companion to pty_master_pollhup_test.c: the child stays alive after
 * dropping its inherited fds, so a wait*-based re-probe could never
 * fire. The parent closes its wrap first; post_close's /proc/self/fd
 * walk releases the parent's real_fd, and the child's raw close of its
 * inherited real_fd drops the last slave ref, so the master POLLHUPs.
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
#include <sys/syscall.h>
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

    struct termios attrs;
    if (tcgetattr(slave, &attrs) != 0)
        err(EXIT_FAILURE, "tcgetattr");
    attrs.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(slave, TCSANOW, &attrs) != 0)
        err(EXIT_FAILURE, "tcsetattr");

    int sync_to_child[2], sync_to_parent[2];
    if (pipe(sync_to_child) != 0 || pipe(sync_to_parent) != 0)
        err(EXIT_FAILURE, "pipe");

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        close(sync_to_child[1]);
        close(sync_to_parent[0]);
        int in = sync_to_child[0];
        int out = sync_to_parent[1];

        // Wait for parent to say "I've closed my wrap; your turn."
        char b;
        if (read(in, &b, 1) != 1)
            _exit(2);

        // Raw close everything in a generous range except the two
        // sync pipes. Going through our shim's close() here would
        // hit relocate_internal, which F_DUPFD's our internal fds
        // out of the way and would defeat the test's intent of
        // forcing all inherited synth + real_fd refs gone.
        for (int fd = 3; fd < 64; fd++) {
            if (fd == in || fd == out)
                continue;
            syscall(SYS_close, fd);
        }

        // Tell parent: "I've released every inherited ref."
        if (write(out, "x", 1) != 1)
            _exit(3);

        // Hold alive forever -- never exit. Parent SIGKILLs us at end.
        pause();
        _exit(0);
    }

    close(sync_to_child[0]);
    close(sync_to_parent[1]);

    // Parent closes its synth wrap FIRST. post_close runs, probe
    // sees the child still holding the inherited synth O_PATH, so
    // tty_release stays untriggered and real_fd persists.
    close(slave);

    // Now tell the child to release everything.
    if (write(sync_to_child[1], "x", 1) != 1)
        err(EXIT_FAILURE, "write sync_to_child");

    char b;
    if (read(sync_to_parent[0], &b, 1) != 1)
        err(EXIT_FAILURE, "read sync_to_parent");

    // At this point the kernel says the synth OFD is dead. A re-probe
    // would succeed and tty_release would close real_fd. wait*-based
    // re-probe can't fire because the child is still alive.
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

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(ep);
    close(master);
    close(sync_to_child[1]);
    close(sync_to_parent[0]);

    if (n > 0) {
        printf("PASS: master events=0x%x\n", got.events);
        return 0;
    }
    printf("FAIL: master never reported events (n=%d)\n", n);
    return 1;
}
