/* Same scenario as pty_childclose_alive_test, but using select() as
 * the wait primitive instead of epoll. The parent closes its wrap
 * before waiting, so post_close's /proc/self/fd walk releases
 * real_fd and the master POLLHUPs once the child drops its refs.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0) err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL) err(EXIT_FAILURE, "ptsname");

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0) err(EXIT_FAILURE, "open slave");

    struct termios attrs;
    if (tcgetattr(slave, &attrs) != 0) err(EXIT_FAILURE, "tcgetattr");
    attrs.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(slave, TCSANOW, &attrs) != 0) err(EXIT_FAILURE, "tcsetattr");

    int sync_to_child[2], sync_to_parent[2];
    if (pipe(sync_to_child) != 0 || pipe(sync_to_parent) != 0)
        err(EXIT_FAILURE, "pipe");

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        close(sync_to_child[1]);
        close(sync_to_parent[0]);
        int in = sync_to_child[0];
        int out = sync_to_parent[1];

        char b;
        if (read(in, &b, 1) != 1) _exit(2);

        for (int fd = 3; fd < 64; fd++) {
            if (fd == in || fd == out) continue;
            syscall(SYS_close, fd);
        }

        if (write(out, "x", 1) != 1) _exit(3);
        pause();
        _exit(0);
    }

    close(sync_to_child[0]);
    close(sync_to_parent[1]);
    close(slave);

    if (write(sync_to_child[1], "x", 1) != 1)
        err(EXIT_FAILURE, "write sync_to_child");
    char b;
    if (read(sync_to_parent[0], &b, 1) != 1)
        err(EXIT_FAILURE, "read sync_to_parent");

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(master, &rfds);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    int r = select(master + 1, &rfds, NULL, NULL, &tv);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(master);
    close(sync_to_child[1]);
    close(sync_to_parent[0]);

    if (r > 0) {
        printf("PASS: master select fired r=%d\n", r);
        return 0;
    }
    printf("FAIL: select(master) returned %d -- no async wakeup from inotify\n", r);
    return 1;
}
