/* Select analog of poll_fork_order_test.
 *
 * Parent provokes inotify_fd open with a throwaway tcsetattr wrap.
 * Parent forks. Child's first synth-relevant call detects pid
 * mismatch, closes inherited inotify, opens a fresh one. Child
 * creates a synth wrap; the OFD destruction event fires on the
 * child's NEW inotify fd. select() in the child must wake.
 *
 * select() splices inotify_fd into rfds on every call, so the
 * registration is built fresh per-call rather than living in a
 * stale set across fork. Expected XPASS. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

    int probe = open(sname, O_RDWR | O_NOCTTY);
    if (probe < 0) err(EXIT_FAILURE, "open probe");
    struct termios attrs;
    if (tcgetattr(probe, &attrs) != 0) err(EXIT_FAILURE, "tcgetattr probe");
    if (tcsetattr(probe, TCSANOW, &attrs) != 0) err(EXIT_FAILURE, "tcsetattr probe");
    close(probe);

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        int slave = open(sname, O_RDWR | O_NOCTTY);
        if (slave < 0) _exit(10);

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

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master, &rfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int r = select(master + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) _exit(0);
        _exit(1);
    }

    close(master);
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS: child select() fired after fork+wrap\n");
        return 0;
    }
    printf("FAIL: child exit status=%d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 1;
}
