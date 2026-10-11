/* Diagnostic for the post-exec wrap restoration. The basic
 * fork+exec path (exec_preserves_tty_test) works, but real-world
 * supervisors (gdb, gnu screen) report EBADF on tcgetattr in the
 * inferior. Run a basic + ptrace + setsid+ctty matrix; whichever
 * subtest fails is the one that diverges from our simple model.
 *
 * Common shape: parent opens a pty, sets raw mode on the slave
 * (triggers tty_install), forks, child dups the wrapped slave onto
 * stdin and execs self with --child. Each scenario layers an extra
 * step in the child between fork and exec.
 *
 * Child exits 0 if tcgetattr(0) succeeds; otherwise it returns an
 * errno-coded status so the parent can identify the failure mode. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <err.h>
#include <valgrind/valgrind.h>

extern char **environ;

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

/* Child mode: exit 0 if stdin is a tty, else encode the failure
 * as an exit code so the parent can tell EBADF from ENOTTY etc. */
enum {
    CHILD_OK      = 0,
    CHILD_EBADF   = 10,
    CHILD_ENOTTY  = 11,
    CHILD_EOTHER  = 20,
    CHILD_SETUP   = 99,
};

static int child_main(void)
{
    struct termios t;
    if (tcgetattr(0, &t) == 0)
        return CHILD_OK;
    switch (errno) {
        case EBADF:   return CHILD_EBADF;
        case ENOTTY:  return CHILD_ENOTTY;
        default:      return CHILD_EOTHER;
    }
}

static const char *describe(int code)
{
    switch (code) {
        case CHILD_OK:     return "stdin IS a tty";
        case CHILD_EBADF:  return "tcgetattr -> EBADF (fd not open or synth O_PATH leaked)";
        case CHILD_ENOTTY: return "tcgetattr -> ENOTTY (fd open but not a tty)";
        case CHILD_EOTHER: return "tcgetattr -> some other errno";
        case CHILD_SETUP:  return "child setup failed before exec";
        default:           return "abnormal exit (signal? non-libc exit?)";
    }
}

static void open_pty(int *m_out, int *s_out)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s == -1) err(EXIT_FAILURE, "open slave");
    *m_out = m;
    *s_out = s;
}

static void set_raw(int fd)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

/* Baseline: same shape as exec_preserves_tty_test. If this fails
 * the regression is in preexec_all itself, not the scenario-specific
 * layering. */
static void test_basic(const char *self)
{
    alarm(10);
    printf("--- basic: fork + dup2 + exec ---\n");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        if (dup2(s, 0) == -1) _exit(CHILD_SETUP);
        char *argv[] = { (char *)self, "--child", NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    close(s);

    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    close(m);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "basic: exit=%d (%s)", code, describe(code));
}

/* gdb's shape: child does PTRACE_TRACEME before execve. Parent
 * must waitpid the post-exec SIGTRAP and PTRACE_CONT. */
static void test_ptrace(const char *self)
{
    alarm(10);
    printf("--- ptrace: PTRACE_TRACEME + exec (gdb pattern) ---\n");
    if (RUNNING_ON_VALGRIND) {
        printf("  SKIP: valgrind can't host PTRACE_TRACEME children\n");
        return;
    }
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) _exit(CHILD_SETUP);
        if (dup2(s, 0) == -1) _exit(CHILD_SETUP);
        char *argv[] = { (char *)self, "--child", NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    close(s);

    /* Catch the post-execve SIGTRAP. */
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid stop");
    if (!WIFSTOPPED(status)) {
        CHECK(0, "ptrace: expected stop, got status=0x%x", status);
        close(m);
        return;
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) err(EXIT_FAILURE, "PTRACE_CONT");

    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid exit");
    close(m);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "ptrace: exit=%d (%s)", code, describe(code));
}

/* screen's shape: child becomes session leader, then makes the
 * slave its controlling tty before duping it onto 0/1/2 and exec. */
static void test_setsid_ctty(const char *self)
{
    alarm(10);
    printf("--- setsid+ctty: new session + TIOCSCTTY + exec (screen pattern) ---\n");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        if (setsid() == -1) _exit(CHILD_SETUP);
        if (ioctl(s, TIOCSCTTY, 0) == -1) _exit(CHILD_SETUP);
        if (dup2(s, 0) == -1) _exit(CHILD_SETUP);
        if (dup2(s, 1) == -1) _exit(CHILD_SETUP);
        if (dup2(s, 2) == -1) _exit(CHILD_SETUP);
        char *argv[] = { (char *)self, "--child", NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    close(s);

    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    close(m);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "setsid+ctty: exit=%d (%s)", code, describe(code));
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: exec_wrap_diag_test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
        return child_main();

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    test_basic(argv[0]);
    test_ptrace(argv[0]);
    test_setsid_ctty(argv[0]);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
