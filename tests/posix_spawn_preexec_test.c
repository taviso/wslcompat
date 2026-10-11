/* Cross-product test for preexec save/restore around posix_spawn.
 *
 * Same axes as exec_preexec_test but the shape differs: posix_spawn
 * clones+execs internally, so we drive it from the main test process
 * and let the spawn shim do the save/restore. The parent's wrap is
 * always checked after every spawn (success or failure) since the
 * posix_spawn shim restores unconditionally.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
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

/* Verify the parent's wrap at fd is functional after a spawn. */
static void parent_wrap_check(int fd, const char *tag)
{
    struct termios t;
    subtests++;
    if (tcgetattr(fd, &t) == 0) {
        printf("  PASS: %s: parent wrap usable after spawn\n", tag);
    } else {
        printf("  FAIL: %s: parent wrap broken after spawn (errno=%d)\n",
               tag, errno);
        failures++;
    }
}

/* --- subtests --- */

/* posix_spawn success, non-CLOEXEC wrap -> child sees a real tty
 * at the wrap's fd number; parent's wrap stays functional. */
static void test_succ_non_cloexec(const char *self)
{
    alarm(10);
    printf("--- success / non-CLOEXEC wrap ---\n");

    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    char arg[16];
    snprintf(arg, sizeof(arg), "%d", s);
    char *argv[] = { (char *)self, "--check-open", arg, NULL };

    pid_t pid;
    int r = posix_spawn(&pid, self, NULL, NULL, argv, environ);
    CHECK(r == 0, "posix_spawn returned %d", r);

    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        CHECK(code == CHILD_OK, "child fd is tty: exit=%d", code);
    }

    parent_wrap_check(s, "non-CLOEXEC");
    close(s);
    close(m);
}

/* posix_spawn with a bad path -> child exec fails (exit 127) or
 * posix_spawn returns ENOENT directly; either way the parent's
 * wrap must be intact afterward (save/restore is unconditional). */
static void test_fail_non_cloexec(void)
{
    alarm(10);
    printf("--- failure / non-CLOEXEC wrap ---\n");

    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    char *argv[] = { "/wslcompat/no/such/path", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, "/wslcompat/no/such/path",
                        NULL, NULL, argv, environ);
    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
        /* child's exec failed -> typically WEXITSTATUS == 127 */
    }

    parent_wrap_check(s, "non-CLOEXEC");
    close(s);
    close(m);
}

/* posix_spawn success, CLOEXEC wrap -> child must not see the fd;
 * parent's wrap stays functional. */
static void test_succ_cloexec(const char *self)
{
    alarm(10);
    printf("--- success / CLOEXEC wrap ---\n");

    int m, s;
    open_pty(&m, &s);
    set_raw(s);
    if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1) err(EXIT_FAILURE, "F_SETFD");

    char arg[16];
    snprintf(arg, sizeof(arg), "%d", s);
    char *argv[] = { (char *)self, "--check-closed", arg, NULL };

    pid_t pid;
    int r = posix_spawn(&pid, self, NULL, NULL, argv, environ);
    CHECK(r == 0, "posix_spawn returned %d", r);

    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        CHECK(code == CHILD_OK, "child fd is closed: exit=%d", code);
    }

    parent_wrap_check(s, "CLOEXEC");
    close(s);
    close(m);
}

/* posix_spawn failure with a CLOEXEC wrap -> wrap is never touched
 * by save (CLOEXEC skipped), so the parent definitely keeps it. */
static void test_fail_cloexec(void)
{
    alarm(10);
    printf("--- failure / CLOEXEC wrap ---\n");

    int m, s;
    open_pty(&m, &s);
    set_raw(s);
    if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1) err(EXIT_FAILURE, "F_SETFD");

    char *argv[] = { "/wslcompat/no/such/path", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, "/wslcompat/no/such/path",
                        NULL, NULL, argv, environ);
    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    }

    parent_wrap_check(s, "CLOEXEC");
    close(s);
    close(m);
}

/* Mixed CLOEXEC/non-CLOEXEC wraps, spawn success -> child sees the
 * expected mask. */
static void test_succ_mixed(const char *self)
{
    alarm(10);
    printf("--- success / mixed wraps ---\n");

    int ms[3], ss[3];
    for (int i = 0; i < 3; i++) {
        open_pty(&ms[i], &ss[i]);
        set_raw(ss[i]);
    }
    if (fcntl(ss[1], F_SETFD, FD_CLOEXEC) == -1) err(EXIT_FAILURE, "F_SETFD");

    char a0[16], a1[16], a2[16];
    snprintf(a0, sizeof(a0), "%d", ss[0]);
    snprintf(a1, sizeof(a1), "%d", ss[1]);
    snprintf(a2, sizeof(a2), "%d", ss[2]);
    char *argv[] = { (char *)self, "--check-mixed", a0, a1, a2, NULL };

    pid_t pid;
    int r = posix_spawn(&pid, self, NULL, NULL, argv, environ);
    CHECK(r == 0, "posix_spawn returned %d", r);

    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        CHECK(code == 5, "mixed mask: expected 5 (0b101), got %d", code);
    }

    for (int i = 0; i < 3; i++) {
        parent_wrap_check(ss[i], "mixed");
        close(ss[i]);
        close(ms[i]);
    }
}

/* Mixed wraps, spawn failure -> parent retains all three wraps. */
static void test_fail_mixed(void)
{
    alarm(10);
    printf("--- failure / mixed wraps ---\n");

    int ms[3], ss[3];
    for (int i = 0; i < 3; i++) {
        open_pty(&ms[i], &ss[i]);
        set_raw(ss[i]);
    }
    if (fcntl(ss[1], F_SETFD, FD_CLOEXEC) == -1) err(EXIT_FAILURE, "F_SETFD");

    char *argv[] = { "/wslcompat/no/such/path", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, "/wslcompat/no/such/path",
                        NULL, NULL, argv, environ);
    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    }

    for (int i = 0; i < 3; i++) {
        parent_wrap_check(ss[i], "mixed");
        close(ss[i]);
        close(ms[i]);
    }
}

/* CLOEXEC laundering via file_actions adddup2: even though the source
 * fd has FD_CLOEXEC set, posix_spawn's child duplicates it onto a
 * destination that is NOT CLOEXEC, so the duplicate survives exec.
 * Save must swap real_fd over the CLOEXEC wrap before the spawn, or
 * the new image inherits the dead synth O_PATH at the destination. */
static void test_succ_cloexec_adddup2(const char *self)
{
    alarm(10);
    printf("--- success / CLOEXEC wrap laundered via adddup2 ---\n");

    int m, s;
    open_pty(&m, &s);
    set_raw(s);
    if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1) err(EXIT_FAILURE, "F_SETFD");

    /* Dup the wrapped CLOEXEC source onto fd 7 in the child. fd 7 in
     * the child will be non-CLOEXEC (dup2 clears it) and survive exec.
     * The check confirms fd 7 is a usable tty in the new image. */
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) err(EXIT_FAILURE, "fa_init");
    if (posix_spawn_file_actions_adddup2(&fa, s, 7) != 0)
        err(EXIT_FAILURE, "adddup2");

    char arg[16];
    snprintf(arg, sizeof(arg), "%d", 7);
    char *argv[] = { (char *)self, "--check-open", arg, NULL };

    pid_t pid;
    int r = posix_spawn(&pid, self, &fa, NULL, argv, environ);
    CHECK(r == 0, "posix_spawn returned %d", r);

    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        CHECK(code == CHILD_OK, "laundered fd is real tty in child: exit=%d", code);
    }

    posix_spawn_file_actions_destroy(&fa);
    parent_wrap_check(s, "CLOEXEC+adddup2");
    close(s);
    close(m);
}

/* posix_spawnp (PATH search variant) -- spot check that the second
 * shim works too. Spawn /bin/true; verify it ran and parent's wrap
 * stays functional. */
static void test_spawnp_basic(void)
{
    alarm(10);
    printf("--- posix_spawnp basic ---\n");

    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    char *argv[] = { "true", NULL };
    pid_t pid;
    int r = posix_spawnp(&pid, "true", NULL, NULL, argv, environ);
    CHECK(r == 0, "posix_spawnp returned %d", r);

    if (r == 0) {
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        CHECK(code == 0, "/bin/true exit=%d", code);
    }

    parent_wrap_check(s, "spawnp");
    close(s);
    close(m);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: posix_spawn_preexec_test timed out\n";
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
    test_succ_cloexec_adddup2(argv[0]);
    test_spawnp_basic();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
