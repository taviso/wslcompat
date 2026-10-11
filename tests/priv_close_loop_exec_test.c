/* Verify the wrap survives the python/portage spawn pattern:
 *
 *   fork()
 *   for fd in 3..1024: close(fd)         <- nukes inherited fds
 *   setuid(unpriv)
 *   execve(...)
 *
 * The close-all loop sweeps forward from fd 3 and closes every
 * inherited fd. Our tty wrap parks its hidden real_fd somewhere
 * >= 3 via F_DUPFD_CLOEXEC, so the loop will eventually hit it.
 * Our tty_relocate handler dups to F_DUPFD_CLOEXEC min=3, which
 * lands on the LOWEST free fd -- which, mid-sweep, is a slot the
 * loop already passed (since the loop just freed it). The cursor
 * never revisits it, so real_fd survives.
 *
 * After execve, our preexec_save runs in the child's execve shim,
 * reads the relocated real_fd, dup3's it over user_fd. The helper
 * inherits a clean real fd at 0.
 *
 * Expected: XPASS. If the helper instead reports [O_PATH wrap] or
 * a canary errno on fd 0, the relocate defense has a hole and
 * close-all is a real failure mode (matters for python's
 * _posixsubprocess.fork_exec and portage's process.spawn).
 *
 * Diagnostics go to stderr (fd 2) which the close loop leaves
 * alone, so no separate report pipe is needed. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

#define CLOSE_LOOP_MAX 1024

static int run_helper(void)
{
    int fail = 0;

    // Raw syscall: libc fcntl on a wrap dispatches to tty_fcntl
    // which forwards to real_fd, hiding the O_PATH bit. Going via
    // syscall() gives the kernel's actual view of fd 0.
    long flags = syscall(SYS_fcntl, 0, F_GETFL);
    if (flags < 0) {
        fprintf(stderr, "[helper] raw fcntl(0, F_GETFL): errno=%d (%m)\n", errno);
        fail++;
    } else {
        fprintf(stderr, "[helper] raw fcntl(0, F_GETFL): 0x%lx%s\n", flags,
                (flags & O_PATH) ? " [O_PATH wrap]" : " [real fd]");
        if (flags & O_PATH)
            fail++;
    }

    struct termios got;
    if (tcgetattr(0, &got) != 0) {
        fprintf(stderr, "[helper] tcgetattr(0): %m\n");
        fail++;
    } else {
        fprintf(stderr, "[helper] tcgetattr(0): OK\n");
    }

    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    int r = poll(&pfd, 1, 2000);
    if (r < 0) {
        fprintf(stderr, "[helper] poll(0): %m\n");
        fail++;
    } else if (r == 0) {
        fprintf(stderr, "[helper] poll(0): timeout (no marker arrived)\n");
        fail++;
    } else if (!(pfd.revents & POLLIN)) {
        fprintf(stderr, "[helper] poll(0): revents=0x%x (no POLLIN)\n", pfd.revents);
        fail++;
    } else {
        char b = '?';
        ssize_t n = read(0, &b, 1);
        if (n != 1 || b != 'M') {
            fprintf(stderr, "[helper] read(0): n=%zd b=0x%02x errno=%m\n",
                    n, (unsigned char)b);
            fail++;
        } else {
            fprintf(stderr, "[helper] read(0): OK ('M')\n");
        }
    }

    return fail;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    alarm(15);

    if (argc >= 2 && strcmp(argv[1], "helper") == 0)
        return run_helper();

    if (geteuid() != 0)
        errx(EXIT_FAILURE, "must be run as root (try `sudo make`)");

    const char *suid = getenv("SUDO_UID");
    const char *sgid = getenv("SUDO_GID");
    if (suid == NULL || sgid == NULL)
        errx(EXIT_FAILURE, "SUDO_UID/SUDO_GID unset (invoke via `sudo make`)");
    uid_t uid = (uid_t)strtoul(suid, NULL, 10);
    gid_t gid = (gid_t)strtoul(sgid, NULL, 10);
    if (uid == 0 || gid == 0)
        errx(EXIT_FAILURE, "refusing to setuid(0)");

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0) err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL) err(EXIT_FAILURE, "ptsname");

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0) err(EXIT_FAILURE, "open slave");

    if (dup2(slave, 0) < 0) err(EXIT_FAILURE, "dup2 stdin");
    close(slave);

    struct termios attrs;
    if (tcgetattr(0, &attrs) != 0) err(EXIT_FAILURE, "tcgetattr");
    attrs.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(0, TCSANOW, &attrs) != 0) err(EXIT_FAILURE, "tcsetattr");

    long pflags = syscall(SYS_fcntl, 0, F_GETFL);
    fprintf(stderr, "[parent] raw fcntl(0, F_GETFL) after tcsetattr: 0x%lx%s\n",
            pflags,
            (pflags >= 0 && (pflags & O_PATH)) ? " [O_PATH wrap]" : " [real fd]");
    fprintf(stderr, "[parent] master fd=%d\n", master);

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Python's _posixsubprocess.fork_exec and portage's process.
        // spawn do exactly this loop between fork() and execve() to
        // prevent fd leakage. We preserve fd 2 (stderr) so we can
        // still report diagnostics; portage doesn't bother. Our
        // internal real_fd / probe_fd dups should all get rescued by
        // the relocate handlers during these closes.
        for (int fd = 3; fd < CLOSE_LOOP_MAX; fd++) {
            if (fd != STDERR_FILENO)
                close(fd);
        }

        if (setgid(gid) != 0) err(70, "setgid(%u)", (unsigned)gid);
        if (setuid(uid) != 0) err(71, "setuid(%u)", (unsigned)uid);

        char *new_argv[] = { argv[0], "helper", NULL };
        execve(argv[0], new_argv, environ);
        err(72, "execve(%s)", argv[0]);
    }

    // Brief settle so the helper reaches poll() before the marker
    // arrives; if we lose the race its 2s poll catches it anyway.
    usleep(100 * 1000);
    if (write(master, "M", 1) != 1)
        warn("write(master, M)");

    int status;
    if (waitpid(pid, &status, 0) < 0) err(EXIT_FAILURE, "waitpid");

    close(master);

    if (!WIFEXITED(status)) {
        printf("FAIL: helper crashed (signal %d)\n", WTERMSIG(status));
        return 1;
    }
    int code = WEXITSTATUS(status);
    if (code != 0) {
        printf("FAIL: %d helper check(s) failed\n", code);
        return 1;
    }

    printf("PASS: wrap survives close(3..%d) + setuid(%u) + execve\n",
           CLOSE_LOOP_MAX - 1, (unsigned)uid);
    return 0;
}
