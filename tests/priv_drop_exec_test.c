/* Cross-privilege synth fd survival.
 *
 * Root parent wraps stdin via tcsetattr (under the shim that calls
 * tty_install). It then fork()s; the child setuid()'s to an unpriv
 * uid and execve()s itself with a "helper" argv to re-enter the test
 * binary in a different role. The helper checks whether stdin is
 * still functional: tcgetattr should succeed, poll should fire for
 * the byte the parent feeds via the master, read should return the
 * marker.
 *
 * What we expect to break:
 *   - setuid(2) clears PR_SET_DUMPABLE; the kernel then transfers
 *     /proc/self ownership to root:root mode 0500.
 *   - Our execve shim runs wslcompat_synth_preexec_save BEFORE the
 *     kernel execve. preexec_save walks /proc/self/fd to find synth
 *     fds; the walk fails with EACCES, so nothing gets unwrapped.
 *   - The child execve'd helper inherits a bare O_PATH wrap with no
 *     synth_table entry. tcgetattr/ioctl on an O_PATH fd returns
 *     EBADF; read/poll fall through to the same canary.
 *
 * Expected behaviour:
 *   - Native Linux: no wrap is ever created, the inherited fd is the
 *     pty slave directly; helper passes. So XPASS on native.
 *   - Under the shim: preexec_save fails after the uid drop -> helper
 *     fails. XFIXME until the shim handles the dumpable transition.
 *
 * Only runs when invoked as root via sudo: we read SUDO_UID/SUDO_GID
 * for the drop-to uid so the unpriv child can still traverse the
 * /mnt/c path to re-exec us (bare `nobody` can't on WSL1's drvfs).
 * The dumpable-clearing setuid behaviour is identical regardless of
 * which non-zero uid we land on, so the bug repro is unchanged. */
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

static int run_helper(int report_fd)
{
    int fail = 0;
    char line[256];

    // Raw syscall bypasses the libc fcntl shim AND the synth-dispatch
    // tty_fcntl handler (which would otherwise forward F_GETFL to the
    // backing real_fd, masking the O_PATH bit on the wrap itself).
    long flags = syscall(SYS_fcntl, 0, F_GETFL);
    if (flags < 0) {
        int e = errno;
        snprintf(line, sizeof line,
                 "raw fcntl(0, F_GETFL): errno=%d (%s)\n", e, strerror(e));
        write(report_fd, line, strlen(line));
    } else {
        snprintf(line, sizeof line,
                 "raw fcntl(0, F_GETFL): 0x%lx%s\n",
                 flags, (flags & O_PATH) ? " [O_PATH wrap]" : " [real fd]");
        write(report_fd, line, strlen(line));
    }

    struct termios got;
    if (tcgetattr(0, &got) != 0) {
        int e = errno;
        snprintf(line, sizeof line,
                 "tcgetattr(0): errno=%d (%s)\n", e, strerror(e));
        write(report_fd, line, strlen(line));
        fail++;
    } else {
        write(report_fd, "tcgetattr(0): OK\n", 17);
    }

    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    int r = poll(&pfd, 1, 2000);
    if (r < 0) {
        int e = errno;
        snprintf(line, sizeof line,
                 "poll(0): errno=%d (%s)\n", e, strerror(e));
        write(report_fd, line, strlen(line));
        fail++;
    } else if (r == 0) {
        write(report_fd, "poll(0): timeout (no marker arrived)\n", 37);
        fail++;
    } else if (!(pfd.revents & POLLIN)) {
        snprintf(line, sizeof line,
                 "poll(0): revents=0x%x (no POLLIN)\n", pfd.revents);
        write(report_fd, line, strlen(line));
        fail++;
    } else {
        char b = '?';
        ssize_t n = read(0, &b, 1);
        if (n != 1 || b != 'M') {
            int e = errno;
            snprintf(line, sizeof line,
                     "read(0): n=%zd byte=0x%02x errno=%d (%s)\n",
                     n, (unsigned char)b, e, strerror(e));
            write(report_fd, line, strlen(line));
            fail++;
        } else {
            write(report_fd, "read(0): OK ('M')\n", 18);
        }
    }

    return fail;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);

    if (argc >= 3 && strcmp(argv[1], "helper") == 0) {
        int fd = atoi(argv[2]);
        return run_helper(fd);
    }

    if (geteuid() != 0) {
        warnx("must be run as root (try `sudo make`)");
        return 1;
    }

    // SUDO_UID/SUDO_GID are set by sudo to the invoking user's ids.
    // We need a uid that can still traverse to argv[0] for the
    // re-exec; on WSL1 /mnt/c, bare `nobody` typically can't.
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
    if (tcgetattr(0, &attrs) != 0) err(EXIT_FAILURE, "parent tcgetattr");
    attrs.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(0, TCSANOW, &attrs) != 0)
        err(EXIT_FAILURE, "parent tcsetattr");

    // Raw fcntl: libc fcntl on a wrapped fd dispatches to tty_fcntl,
    // which forwards to the backing real_fd and hides the O_PATH bit.
    // Going via syscall() lets us see the kernel's real view of fd 0.
    long pflags = syscall(SYS_fcntl, 0, F_GETFL);
    printf("[parent] raw fcntl(0, F_GETFL) after tcsetattr: 0x%lx%s\n",
           pflags,
           (pflags >= 0 && (pflags & O_PATH)) ? " [O_PATH wrap]" : " [real fd]");

    // Backchannel: helper writes its diagnostics here so the test
    // captures them even when stdout is broken. Read end stays in
    // parent only.
    int rep[2];
    if (pipe(rep) != 0) err(EXIT_FAILURE, "report pipe");

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        close(rep[0]);

        char errline[256];

        if (setgid(gid) != 0) {
            int e = errno;
            snprintf(errline, sizeof errline, "setgid(%u): errno=%d (%s)\n",
                     (unsigned)gid, e, strerror(e));
            write(rep[1], errline, strlen(errline));
            _exit(70);
        }
        if (setuid(uid) != 0) {
            int e = errno;
            snprintf(errline, sizeof errline, "setuid(%u): errno=%d (%s)\n",
                     (unsigned)uid, e, strerror(e));
            write(rep[1], errline, strlen(errline));
            _exit(71);
        }
        if (getuid() != uid || geteuid() != uid) {
            snprintf(errline, sizeof errline,
                     "uid mismatch after setuid: uid=%u euid=%u (wanted %u)\n",
                     (unsigned)getuid(), (unsigned)geteuid(), (unsigned)uid);
            write(rep[1], errline, strlen(errline));
            _exit(72);
        }

        char fdarg[16];
        snprintf(fdarg, sizeof fdarg, "%d", rep[1]);

        char *new_argv[] = { argv[0], "helper", fdarg, NULL };
        execve(argv[0], new_argv, environ);

        // Execve returned -> it failed. Report why before giving up.
        int e = errno;
        snprintf(errline, sizeof errline,
                 "execve(\"%s\"): errno=%d (%s)\n", argv[0], e, strerror(e));
        write(rep[1], errline, strlen(errline));
        _exit(73);
    }

    close(rep[1]);

    // Feed the marker the helper's poll() is waiting for. Small
    // settle so we don't race the helper's startup; if we lose
    // anyway the helper's 2s poll timeout still catches it.
    usleep(100 * 1000);
    if (write(master, "M", 1) != 1)
        warn("write(master, M)");

    // Wait for the helper to finish, THEN drain its diagnostic pipe.
    // Reading before waitpid risks returning on the first write and
    // losing every subsequent line.
    int status;
    if (waitpid(pid, &status, 0) < 0) err(EXIT_FAILURE, "waitpid");

    printf("[helper]\n");
    for (;;) {
        char buf[4096];
        ssize_t n = read(rep[0], buf, sizeof buf - 1);
        if (n <= 0)
            break;
        buf[n] = '\0';
        fputs(buf, stdout);
    }

    close(master);
    close(rep[0]);

    if (!WIFEXITED(status)) {
        printf("FAIL: helper crashed (signal %d)\n", WTERMSIG(status));
        return 1;
    }

    int code = WEXITSTATUS(status);
    if (code != 0) {
        printf("FAIL: helper reported %d failure(s) (exit=%d)\n", code, code);
        return 1;
    }

    printf("PASS: stdin tty wrap survives setuid(%u)+execve\n", (unsigned)uid);
    return 0;
}
