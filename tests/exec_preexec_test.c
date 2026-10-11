/* Cross-product test for the preexec save/restore mechanism.
 *
 * Axes:
 *   - {non-CLOEXEC, CLOEXEC} synth wrap
 *   - {exec success, exec failure} via execve
 *   - {single wrap, mixed multi-wrap}
 *
 * Expectations under the shim:
 *   - non-CLOEXEC wrap, exec success -> child fd is a real tty
 *   - non-CLOEXEC wrap, exec failure -> parent's wrap is still functional
 *   - CLOEXEC wrap, exec success     -> child fd is absent (EBADF)
 *   - CLOEXEC wrap, exec failure     -> parent's wrap is still functional
 *
 * The same observable behavior also holds on a plain kernel: TCSETS
 * doesn't install a wrap, but the kernel tty is functional and CLOEXEC
 * still closes the fd. So this is an XPASS test.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

/* Child exit codes -- 0 is success; non-zero values encode the failure
 * mode so the parent can distinguish EBADF from ENOTTY etc. */
enum {
    CHILD_OK         = 0,
    CHILD_ENOTTY     = 11,
    CHILD_EBADF      = 12,
    CHILD_UNEXPECTED = 13,
    CHILD_EOTHER     = 20,
    CHILD_SETUP      = 99,
};

static int child_check_open(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) == 0)
        return CHILD_OK;
    switch (errno) {
        case EBADF:  return CHILD_EBADF;
        case ENOTTY: return CHILD_ENOTTY;
        default:     return CHILD_EOTHER;
    }
}

static int child_check_closed(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) == -1 && errno == EBADF)
        return CHILD_OK;
    return CHILD_UNEXPECTED;
}

/* Mixed mode: each remaining argv is an fd. Return bitmask: bit i set
 * iff fd[i] is a usable tty in the child. */
static int child_check_mixed(int n, char **argv)
{
    int mask = 0;
    for (int i = 0; i < n; i++) {
        int fd = atoi(argv[i]);
        struct termios t;
        if (tcgetattr(fd, &t) == 0)
            mask |= (1 << i);
    }
    return mask;
}

static int child_main(int argc, char **argv)
{
    const char *mode = argv[1];
    if (strcmp(mode, "--check-open") == 0 && argc >= 3)
        return child_check_open(atoi(argv[2]));
    if (strcmp(mode, "--check-closed") == 0 && argc >= 3)
        return child_check_closed(atoi(argv[2]));
    if (strcmp(mode, "--check-mixed") == 0 && argc >= 3)
        return child_check_mixed(argc - 2, argv + 2);
    return CHILD_SETUP;
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

/* --- subtests --- */

/* exec success, non-CLOEXEC wrap -> child sees a usable tty at the
 * wrap's fd number. Validates that preexec_save actually swapped
 * real_fd over user_fd before the new image started. */
static void test_succ_non_cloexec(const char *self)
{
    alarm(10);
    printf("--- success / non-CLOEXEC wrap ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        char arg[16];
        snprintf(arg, sizeof(arg), "%d", s);
        char *argv[] = { (char *)self, "--check-open", arg, NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "child fd is tty: exit=%d", code);
}

/* exec failure, non-CLOEXEC wrap -> the parent (child of fork)
 * comes back from execve with errno set; the wrap must still be
 * usable. Validates preexec_restore. */
static void test_fail_non_cloexec(void)
{
    alarm(10);
    printf("--- failure / non-CLOEXEC wrap ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);

        char *argv[] = { "/wslcompat/no/such/path", NULL };
        execve("/wslcompat/no/such/path", argv, environ);
        if (errno != ENOENT) _exit(CHILD_SETUP);

        _exit(child_check_open(s));
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "wrap usable after failed exec: exit=%d", code);
}

/* exec success, CLOEXEC wrap -> child must not see the fd. The shim
 * should skip CLOEXEC wraps in preexec_save (the kernel closes them
 * during exec), so the new image sees nothing at that fd number. */
static void test_succ_cloexec(const char *self)
{
    alarm(10);
    printf("--- success / CLOEXEC wrap ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1) _exit(CHILD_SETUP);
        char arg[16];
        snprintf(arg, sizeof(arg), "%d", s);
        char *argv[] = { (char *)self, "--check-closed", arg, NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "child fd is closed: exit=%d", code);
}

/* exec failure, CLOEXEC wrap -> parent's wrap is untouched because
 * preexec_save skipped it; it must still be functional. */
static void test_fail_cloexec(void)
{
    alarm(10);
    printf("--- failure / CLOEXEC wrap ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1) _exit(CHILD_SETUP);

        char *argv[] = { "/wslcompat/no/such/path", NULL };
        execve("/wslcompat/no/such/path", argv, environ);
        if (errno != ENOENT) _exit(CHILD_SETUP);

        _exit(child_check_open(s));
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "wrap intact after failed exec: exit=%d", code);
}

/* exec success, three wraps with the middle one CLOEXEC -> child
 * sees fd[0] and fd[2] but not fd[1]. Bitmask 0b101 = 5. */
static void test_succ_mixed(const char *self)
{
    alarm(10);
    printf("--- success / mixed wraps ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int ms[3], ss[3];
        for (int i = 0; i < 3; i++) {
            open_pty(&ms[i], &ss[i]);
            set_raw(ss[i]);
        }
        if (fcntl(ss[1], F_SETFD, FD_CLOEXEC) == -1) _exit(CHILD_SETUP);

        char a0[16], a1[16], a2[16];
        snprintf(a0, sizeof(a0), "%d", ss[0]);
        snprintf(a1, sizeof(a1), "%d", ss[1]);
        snprintf(a2, sizeof(a2), "%d", ss[2]);
        char *argv[] = { (char *)self, "--check-mixed", a0, a1, a2, NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == 5, "mixed mask: expected 5 (0b101), got %d", code);
}

/* exec failure, three wraps mixed CLOEXEC/non -> all three must
 * remain usable in the parent. */
static void test_fail_mixed(void)
{
    alarm(10);
    printf("--- failure / mixed wraps ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int ms[3], ss[3];
        for (int i = 0; i < 3; i++) {
            open_pty(&ms[i], &ss[i]);
            set_raw(ss[i]);
        }
        if (fcntl(ss[1], F_SETFD, FD_CLOEXEC) == -1) _exit(CHILD_SETUP);

        char *argv[] = { "/wslcompat/no/such/path", NULL };
        execve("/wslcompat/no/such/path", argv, environ);
        if (errno != ENOENT) _exit(CHILD_SETUP);

        for (int i = 0; i < 3; i++) {
            struct termios t;
            if (tcgetattr(ss[i], &t) != 0)
                _exit(CHILD_EOTHER + i);
        }
        _exit(CHILD_OK);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "all wraps usable after failed exec: exit=%d", code);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: exec_preexec_test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2 && strncmp(argv[1], "--", 2) == 0)
        return child_main(argc, argv);

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    test_succ_non_cloexec(argv[0]);
    test_fail_non_cloexec();
    test_succ_cloexec(argv[0]);
    test_fail_cloexec();
    test_succ_mixed(argv[0]);
    test_fail_mixed();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
