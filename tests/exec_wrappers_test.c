/* Spot-check that the libc exec wrappers go through our preexec
 * machinery. Each wrapper eventually funnels into execve (or our
 * fexecve shim, or sh -> execve), so a single non-CLOEXEC wrap
 * survives into the new image if the shim was reached.
 *
 * Covers: execvp, execlp, fexecve, system, popen.
 * Skipped: execv/execl/execle/execvpe -- mechanically equivalent
 * to execvp/execlp from the shim's perspective.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "testutil.h"

extern char **environ;

static char self_path[PATH_MAX];

enum {
    CHILD_OK         = 0,
    CHILD_ENOTTY     = 11,
    CHILD_EBADF      = 12,
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

static int child_main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--check-open") == 0)
        return child_check_open(atoi(argv[2]));
    return CHILD_SETUP;
}

static void resolve_self(void)
{
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n < 0) err(EXIT_FAILURE, "readlink /proc/self/exe");
    self_path[n] = 0;
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

static int wait_child(pid_t pid)
{
    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* --- subtests --- */

static void test_execvp(void)
{
    alarm(10);
    STATUS("--- execvp ---");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        char arg[16];
        snprintf(arg, sizeof(arg), "%d", s);
        char *argv[] = { self_path, "--check-open", arg, NULL };
        execvp(self_path, argv);
        _exit(CHILD_SETUP);
    }
    int code = wait_child(pid);
    CHECK(code == CHILD_OK, "execvp: exit=%d", code);
}

static void test_execlp(void)
{
    alarm(10);
    STATUS("--- execlp ---");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        char arg[16];
        snprintf(arg, sizeof(arg), "%d", s);
        execlp(self_path, self_path, "--check-open", arg, (char *)NULL);
        _exit(CHILD_SETUP);
    }
    int code = wait_child(pid);
    CHECK(code == CHILD_OK, "execlp: exit=%d", code);
}

static void test_fexecve(void)
{
    alarm(10);
    STATUS("--- fexecve ---");
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int m, s;
        open_pty(&m, &s);
        set_raw(s);
        int bfd = open(self_path, O_RDONLY | O_CLOEXEC);
        if (bfd == -1) _exit(CHILD_SETUP);
        char arg[16];
        snprintf(arg, sizeof(arg), "%d", s);
        char *argv[] = { self_path, "--check-open", arg, NULL };
        fexecve(bfd, argv, environ);
        _exit(CHILD_SETUP);
    }
    int code = wait_child(pid);
    CHECK(code == CHILD_OK, "fexecve: exit=%d", code);
}

/* gdb's pattern: fork, close every fd 3..rlim in the child to avoid
 * leaking debugger-internal fds, then exec the inferior. Our backing
 * real_fd lives in that range -- without close-shim's relocate the
 * preexec dup3 would EBADF and the new image inherits a dead synth
 * O_PATH. Master is kept alive in the parent so the pty pair survives
 * the child's close-all. */
static void test_close_all_then_exec(void)
{
    alarm(10);
    STATUS("--- fork + close-all + exec (gdb pattern) ---");
    int m, s;
    open_pty(&m, &s);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        /* Move slave onto stdin (fd 0 stays alive across close-all),
         * drop the master ref in the child -- parent retains one. */
        if (dup2(s, 0) == -1) _exit(CHILD_SETUP);
        close(m);
        close(s);
        set_raw(0);    /* installs wrap on fd 0 */

        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(CHILD_SETUP);
        int hi = rl.rlim_cur < 1024 ? (int)rl.rlim_cur : 1024;
        for (int i = 3; i < hi; i++)
            close(i);

        char *argv[] = { self_path, "--check-open", "0", NULL };
        execvp(self_path, argv);
        _exit(CHILD_SETUP);
    }
    int code = wait_child(pid);
    CHECK(code == CHILD_OK,
          "close-all + exec: fd 0 still a tty in child: exit=%d", code);
    close(m);
    close(s);
}

/* vfork-CLONE_VM: shared memory but separate fd table. A shim that
 * naively mutates shared real_fd / probe_fd in the child corrupts the
 * parent's wrap when it resumes. This is the gdb pattern (gdb vforks
 * its inferior). Parent wraps a tty, vforks a child that does
 * close-all + exec, parent verifies its wrap still works. */
static void test_vfork_close_all_preserves_parent(void)
{
    alarm(10);
    STATUS("--- vfork + close-all + exec preserves parent wrap ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);    /* installs wrap on fd s */

    pid_t pid = vfork();
    if (pid == -1) err(EXIT_FAILURE, "vfork");
    if (pid == 0) {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(CHILD_SETUP);
        int hi = rl.rlim_cur < 1024 ? (int)rl.rlim_cur : 1024;
        for (int i = 3; i < hi; i++)
            close(i);
        char *argv[] = { "/bin/true", NULL };
        execve("/bin/true", argv, environ);
        _exit(CHILD_SETUP);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");

    struct termios t;
    int ok = (tcgetattr(s, &t) == 0);
    CHECK(ok, "parent's tcgetattr works after vfork child: errno=%d",
          ok ? 0 : errno);

    close(s);
    close(m);
}

/* vfork variant of test_close_all_then_exec: verify the CHILD (not
 * just the parent) sees a real tty after exec. This only succeeds
 * if preexec_save resolved real_fd via the per-pid override slot,
 * because close-all destroyed the base real_fd in the child's fd
 * table. */
static void test_vfork_close_all_child_sees_tty(void)
{
    alarm(10);
    STATUS("--- vfork + close-all + exec: child sees real tty ---");
    int m, s;
    open_pty(&m, &s);

    pid_t pid = vfork();
    if (pid == -1) err(EXIT_FAILURE, "vfork");
    if (pid == 0) {
        /* vfork child can't modify its own state safely (shared VM),
         * but dup2 + setraw + close-all + exec is the gdb pattern. */
        if (dup2(s, 0) == -1) _exit(CHILD_SETUP);
        set_raw(0);
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(CHILD_SETUP);
        int hi = rl.rlim_cur < 1024 ? (int)rl.rlim_cur : 1024;
        for (int i = 3; i < hi; i++)
            close(i);
        char *argv[] = { self_path, "--check-open", "0", NULL };
        execvp(self_path, argv);
        _exit(CHILD_SETUP);
    }
    int code = wait_child(pid);
    CHECK(code == CHILD_OK,
          "vfork+close-all+exec: child fd 0 is a tty: exit=%d", code);

    close(m);
    close(s);
}

/* Stress test for the override slot GC. Each vfork leaves a dead-pid
 * override slot. With only SYNTH_OVERRIDE_SLOTS (=4) slots per entry,
 * the 5th vfork would exhaust them if GC didn't reclaim dead ones.
 * Running well past 4 confirms acquire-time GC fires. */
static void test_vfork_stress_gc(void)
{
    alarm(30);
    STATUS("--- 50 vforks force GC of dead override slots ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    int passed = 0;
    for (int iter = 0; iter < 50; iter++) {
        pid_t pid = vfork();
        if (pid == -1) err(EXIT_FAILURE, "vfork");
        if (pid == 0) {
            struct rlimit rl;
            if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(CHILD_SETUP);
            int hi = rl.rlim_cur < 1024 ? (int)rl.rlim_cur : 1024;
            for (int i = 3; i < hi; i++)
                close(i);
            char *argv[] = { "/bin/true", NULL };
            execve("/bin/true", argv, environ);
            _exit(CHILD_SETUP);
        }
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");

        struct termios t;
        if (tcgetattr(s, &t) != 0)
            break;
        passed++;
    }
    CHECK(passed == 50,
          "parent wrap survived %d/50 vforks (override GC working)",
          passed);

    close(s);
    close(m);
}

/* Same shape, run several times in a row. Each vfork child leaves a
 * stale per-pid override in shared state; the parent's wrap must stay
 * usable across all of them. */
static void test_vfork_repeated(void)
{
    alarm(30);
    STATUS("--- repeated vfork+close-all+exec preserves parent wrap ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    int passed_iters = 0;
    for (int iter = 0; iter < 8; iter++) {
        pid_t pid = vfork();
        if (pid == -1) err(EXIT_FAILURE, "vfork");
        if (pid == 0) {
            struct rlimit rl;
            if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(CHILD_SETUP);
            int hi = rl.rlim_cur < 1024 ? (int)rl.rlim_cur : 1024;
            for (int i = 3; i < hi; i++)
                close(i);
            char *argv[] = { "/bin/true", NULL };
            execve("/bin/true", argv, environ);
            _exit(CHILD_SETUP);
        }
        int status;
        if (waitpid(pid, &status, 0) == -1) err(EXIT_FAILURE, "waitpid");

        struct termios t;
        if (tcgetattr(s, &t) != 0)
            break;
        passed_iters++;
    }
    CHECK(passed_iters == 8,
          "parent wrap survived %d/8 sequential vforks", passed_iters);

    close(s);
    close(m);
}

/* system() handles its own fork+exec. The wrap stays in the test
 * process; the sh child inherits it (synth_table copy) and exec's
 * /bin/sh, which triggers our shim and swaps the wrap. */
static void test_system(void)
{
    alarm(10);
    STATUS("--- system ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "%s --check-open %d", self_path, s);
    int r = system(cmd);
    int code = WIFEXITED(r) ? WEXITSTATUS(r) : -1;
    CHECK(code == CHILD_OK, "system: exit=%d", code);

    close(s);
    close(m);
}

/* popen() forks and pipes stdout, but the wrap at fd s is unrelated
 * to the pipe redirect on fd 1, so it survives the same way. */
static void test_popen(void)
{
    alarm(10);
    STATUS("--- popen ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "%s --check-open %d", self_path, s);
    FILE *fp = popen(cmd, "r");
    if (!fp) err(EXIT_FAILURE, "popen");
    char buf[64];
    while (fgets(buf, sizeof(buf), fp))
        ; /* drain */
    int r = pclose(fp);
    int code = WIFEXITED(r) ? WEXITSTATUS(r) : -1;
    CHECK(code == CHILD_OK, "popen: exit=%d", code);

    close(s);
    close(m);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: exec_wrappers_test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2 && strncmp(argv[1], "--", 2) == 0)
        return child_main(argc, argv);

    resolve_self();

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    test_execvp();
    test_execlp();
    test_fexecve();
    test_close_all_then_exec();
    test_vfork_close_all_preserves_parent();
    test_vfork_close_all_child_sees_tty();
    test_vfork_repeated();
    test_vfork_stress_gc();
    test_system();
    test_popen();

    TEST_REPORT();
}
