/* Verify that a tty wrap survives execve. Without preexec_all in
 * synth.c, a child that inherits a wrapped fd (synth O_PATH backed
 * by a process-local real_fd) sees a dead fd post-exec: real_fd is
 * CLOEXEC and dies, leaving the synth O_PATH pointing at a tmpfile
 * inode the child can't service. tcgetattr / isatty / etc. then
 * fail with EBADF.
 *
 * Setup: open a pty, set raw mode on the slave (triggers our
 * tty_install), fork, dup2 the wrapped slave onto stdin, exec self
 * with a sentinel arg. The child checks isatty(0) and exits 0 if
 * it sees a tty. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <err.h>

extern char **environ;

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static int child_main(void)
{
    /* Child checks: is my stdin a tty? Without preexec restoration
     * this returns 0 because the inherited synth O_PATH can't be
     * tcgetattr'd. */
    if (isatty(0))
        return 0;
    fprintf(stderr, "child: stdin is NOT a tty (errno=%d)\n", errno);
    return 1;
}

static void open_pty(int *master_fd, int *slave_fd)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s == -1) err(EXIT_FAILURE, "open slave");
    *master_fd = m;
    *slave_fd  = s;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
        return child_main();

    int m, s;
    open_pty(&m, &s);

    /* Triggers tty_install on s. */
    struct termios t;
    if (ioctl(s, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    if (ioctl(s, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        /* Child: redirect the wrapped slave onto stdin and exec self. */
        if (dup2(s, 0) == -1) {
            perror("child dup2");
            _exit(2);
        }
        char *new_argv[] = { argv[0], "--child", NULL };
        execve(argv[0], new_argv, environ);
        perror("child execve");
        _exit(3);
    }

    close(s);
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");

    int exited = WIFEXITED(status);
    int code   = exited ? WEXITSTATUS(status) : -1;
    CHECK(exited && code == 0,
          "child saw real tty after exec (status=0x%x exit=%d)", status, code);

    close(m);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
