/* Test graceful degradation when RLIMIT_NOFILE is too low for
 * preexec_save's F_DUPFD_CLOEXEC(fd, SYNTH_PREEXEC_MAX=64) park.
 *
 * Under the shim with a low rlimit, save's F_DUPFD returns EMFILE,
 * the slot is skipped, and the wrap is "lost" in the new image -- but
 * nothing crashes, parent's wrap is untouched (save never swapped it),
 * and exec proceeds normally.
 *
 * XPASS because the observable outcomes we test (exec ran, parent's
 * wrap still functional on failure) also hold without the shim --
 * there's just no wrap to lose.
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
#include <sys/resource.h>
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

enum {
    CHILD_OK         = 0,
    CHILD_EBADF      = 12,
    CHILD_EOTHER     = 20,
    CHILD_SETUP      = 99,
};

static int child_check_open(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) == 0)
        return CHILD_OK;
    if (errno == EBADF) return CHILD_EBADF;
    return CHILD_EOTHER;
}

/* --check-ran: prove we got here. Any non-crash exit means save's
 * failure path didn't take the process down. */
static int child_main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--check-ran") == 0)
        return CHILD_OK;
    if (argc >= 3 && strcmp(argv[1], "--check-open") == 0)
        return child_check_open(atoi(argv[2]));
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

/* Lower RLIMIT_NOFILE to something below SYNTH_PREEXEC_MAX so save's
 * F_DUPFD_CLOEXEC(fd, 64) returns EMFILE. */
static int lower_nofile(rlim_t to)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;
    rl.rlim_cur = to;
    return setrlimit(RLIMIT_NOFILE, &rl);
}

/* Successful exec under low rlimit -> child reaches the new image
 * and exits cleanly. Save's F_DUPFD failure must not crash. */
static void test_succ_low_rlimit(const char *self)
{
    alarm(10);
    printf("--- success / low rlimit (no crash) ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        if (lower_nofile(32) != 0) _exit(CHILD_SETUP);

        char *argv[] = { (char *)self, "--check-ran", NULL };
        execve(self, argv, environ);
        _exit(CHILD_SETUP);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code == CHILD_OK, "low-rlimit exec ran cleanly: exit=%d", code);
}

/* Failed exec under low rlimit -> parent's wrap stays functional.
 * Save couldn't swap (F_DUPFD failed), so there's nothing to restore
 * and user_fd was never touched. */
static void test_fail_low_rlimit(void)
{
    alarm(10);
    printf("--- failure / low rlimit (wrap untouched) ---\n");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        if (lower_nofile(32) != 0) _exit(CHILD_SETUP);

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

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: exec_rlimit_test timed out\n";
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

    test_succ_low_rlimit(argv[0]);
    test_fail_low_rlimit();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
