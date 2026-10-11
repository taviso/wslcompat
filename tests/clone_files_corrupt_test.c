/* XFIXME: CLONE_FILES child shares the kernel fd table but NOT the
 * synth_table (separate memory). When the child closes one of our
 * internal fds (probe_fd, tty real_fd), the parent's synth_table
 * still records the old fd number, but the kernel-level entry is
 * gone -- shared by both processes.
 *
 * Symptom: parent writes/reads to the wrapped tty fail with EBADF
 * because tty_passthru returns real_fd which the child closed in
 * the shared kernel table.
 *
 * Unshimmed: no wrap, no internal fds; the child closing high fd
 * numbers is a no-op for the test's flow. The slave's write succeeds.
 *
 * Shimmed: the wrap exists, the child closes our internals via the
 * shared fd table, and the parent's write fails. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <err.h>

#include "testutil.h"

// Snapshot fd numbers from /proc/self/fd. Returns count. Skips the
// dirfd we hold for the walk.
static int snapshot_fds(int *out, size_t cap)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) && (size_t)n < cap) {
        if (e->d_name[0] == '.')
            continue;
        int fd = atoi(e->d_name);
        if (fd == dirfd(d))
            continue;
        out[n++] = fd;
    }
    closedir(d);
    return n;
}

// Compute b \ a (fds in b not in a). Returns count written to out.
static int diff_fds(const int *a, int na, const int *b, int nb,
                    int *out, size_t cap)
{
    int n = 0;
    for (int j = 0; j < nb && (size_t)n < cap; j++) {
        int found = 0;
        for (int i = 0; i < na; i++)
            if (a[i] == b[j]) { found = 1; break; }
        if (!found)
            out[n++] = b[j];
    }
    return n;
}

// Args passed from parent to clone-child.
struct child_args {
    int *fds_to_close;
    int  n_fds;
    int  done_pipe_w;
};

static int child_fn(void *arg)
{
    struct child_args *a = arg;
    for (int i = 0; i < a->n_fds; i++)
        close(a->fds_to_close[i]);
    write(a->done_pipe_w, "x", 1);
    _exit(0);
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    int snap_before[256], snap_after[256], internals[256];
    int n_before, n_after, n_internals;

    int master, slave;
    open_pty(&master, &slave);

    n_before = snapshot_fds(snap_before, 256);

    // Trigger our tty wrap on the slave. After this, snap_after will
    // contain the slave canary + our internal probe_fd/real_fd.
    struct termios t;
    if (tcgetattr(slave, &t) != 0) err(EXIT_FAILURE, "tcgetattr");
    t.c_lflag &= ~ICANON;
    if (tcsetattr(slave, TCSANOW, &t) != 0) err(EXIT_FAILURE, "tcsetattr");

    n_after = snapshot_fds(snap_after, 256);
    n_internals = diff_fds(snap_before, n_before, snap_after, n_after,
                           internals, 256);
    printf("  INFO: wrap created %d new fd(s)\n", n_internals);

    // Spawn a CLONE_FILES child that closes our newly-created fds.
    char *stack = aligned_alloc(16, 1 << 16);
    if (!stack) err(EXIT_FAILURE, "aligned_alloc");

    int done_pipe[2];
    if (pipe(done_pipe) != 0) err(EXIT_FAILURE, "pipe");

    struct child_args args = {
        .fds_to_close = internals,
        .n_fds        = n_internals,
        .done_pipe_w  = done_pipe[1],
    };

    pid_t pid = clone(child_fn, stack + (1 << 16),
                      CLONE_FILES | SIGCHLD, &args);
    if (pid == -1) err(EXIT_FAILURE, "clone(CLONE_FILES)");

    char b;
    if (read(done_pipe[0], &b, 1) != 1)
        err(EXIT_FAILURE, "wait for child done");

    int wstatus;
    if (waitpid(pid, &wstatus, 0) != pid)
        err(EXIT_FAILURE, "waitpid");
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
          "child exited cleanly");

    // The bug: parent's wrap is now broken because internal fds got
    // closed in the shared kernel table. Verify writing to the slave
    // round-trips through the pty.
    const char msg[] = "ping";
    ssize_t w = write(slave, msg, sizeof(msg));
    CHECK(w == sizeof(msg),
          "write to wrapped slave: got %zd expected %zu (%s)",
          w, sizeof(msg), w < 0 ? strerror(errno) : "");

    if (w == sizeof(msg)) {
        char rbuf[64];
        ssize_t r = read(master, rbuf, sizeof(rbuf));
        CHECK(r == sizeof(msg) && memcmp(rbuf, msg, sizeof(msg)) == 0,
              "master read back %zd bytes", r);
    }

    free(stack);
    close(done_pipe[0]);
    close(done_pipe[1]);
    close(master);

    TEST_REPORT();
}
