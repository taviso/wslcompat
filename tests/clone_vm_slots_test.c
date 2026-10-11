/* XFIXME: synth's per-entry CoW override array has SYNTH_OVERRIDE_SLOTS
 * (= 4) slots. With raw clone(CLONE_VM) -- like vfork but no
 * obligation for the child to exec/exit quickly -- a parent can spawn
 * more concurrent VM-sharing children than there are slots, and the
 * later ones fail to claim a slot. Their view of real_fd falls back
 * to the base value, which may already be stale in the child's own
 * fd table (because the child closed the inherited number).
 *
 * Symptom: the 5th+ concurrent CLONE_VM child can't use the wrapped
 * tty -- read/write on the synth fd returns EBADF because tty_passthru
 * gives back a fd number that's closed in the child's table.
 *
 * Unshimmed: no wrap, no slot machinery; every child succeeds.
 *
 * Shimmed: only SYNTH_OVERRIDE_SLOTS children succeed.
 *
 * Spawns 8 children, synchronized via a barrier pipe so they all hold
 * their slot at once (otherwise GC would reclaim and the test would
 * never observe exhaustion). */
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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <err.h>

#define N_CHILDREN 8

#include "testutil.h"

// Snapshot fds (skip dirfd we hold for the walk).
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

struct child_args {
    int  slave_fd;
    int  ready_pipe_w;     // child signals "I tried" + result
    int  release_pipe_r;   // child waits for permission to exit
    int  internal_fd;      // an fd number whose close triggers relocate
};

// Child closes an internal fd to force tty_relocate, then attempts
// a tcgetattr on the wrapped slave (which routes through tty_ioctl
// -> tty_passthru -> the post-relocate real_fd view). Reports
// success/failure via a single byte over ready_pipe_w.
static int child_fn(void *arg)
{
    struct child_args *a = arg;

    // Try to close all inherited internal fds. Each close goes
    // through our shim and triggers per-entry relocate; for the
    // entry whose real_fd matches, tty_real_fd_store claims a CoW
    // slot for this child's pid.
    close(a->internal_fd);

    // Now exercise the wrap. If our slot claim succeeded, the
    // dispatch resolves real_fd via the CoW slot. If it failed,
    // tty_real_fd_load returns the base value (parent's fd number),
    // which we just closed in our table -- so the ioctl fails.
    struct termios t;
    int r = tcgetattr(a->slave_fd, &t);
    char status = (r == 0) ? 'P' : 'F';
    write(a->ready_pipe_w, &status, 1);

    // Hold our slot until parent says we can exit. Otherwise GC at
    // override_acquire reclaims us and slot exhaustion never bites.
    char go;
    read(a->release_pipe_r, &go, 1);
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
    alarm(15);

    int master, slave;
    open_pty(&master, &slave);

    int snap_before[256], snap_after[256];
    int n_before = snapshot_fds(snap_before, 256);

    // Trigger our tty wrap.
    struct termios t;
    if (tcgetattr(slave, &t) != 0) err(EXIT_FAILURE, "tcgetattr");
    t.c_lflag &= ~ICANON;
    if (tcsetattr(slave, TCSANOW, &t) != 0) err(EXIT_FAILURE, "tcsetattr");

    int n_after = snapshot_fds(snap_after, 256);

    // Find one of our internal fds (any fd in after-set but not
    // before-set). Children close it to trigger relocate.
    int internal_fd = -1;
    for (int j = 0; j < n_after; j++) {
        int found = 0;
        for (int i = 0; i < n_before; i++)
            if (snap_before[i] == snap_after[j]) { found = 1; break; }
        if (!found) { internal_fd = snap_after[j]; break; }
    }
    if (internal_fd < 0) {
        // No wrap was created (unshimmed). Children have nothing to
        // close, so their tcgetattr will trivially succeed. The test
        // still spawns children to keep the unshimmed path the same
        // shape; just no real exercise.
        printf("  INFO: no internal fd detected (unshimmed run)\n");
        internal_fd = -1;
    } else {
        printf("  INFO: using internal_fd=%d as relocate trigger\n",
               internal_fd);
    }

    int ready_pipe[2];
    int release_pipe[2];
    if (pipe(ready_pipe) != 0)   err(EXIT_FAILURE, "pipe");
    if (pipe(release_pipe) != 0) err(EXIT_FAILURE, "pipe");

    pid_t kids[N_CHILDREN];
    char  *stacks[N_CHILDREN];

    struct child_args args = {
        .slave_fd       = slave,
        .ready_pipe_w   = ready_pipe[1],
        .release_pipe_r = release_pipe[0],
        .internal_fd    = internal_fd,
    };

    for (int i = 0; i < N_CHILDREN; i++) {
        stacks[i] = aligned_alloc(16, 1 << 16);
        if (!stacks[i]) err(EXIT_FAILURE, "aligned_alloc");
        kids[i] = clone(child_fn, stacks[i] + (1 << 16),
                        CLONE_VM | SIGCHLD, &args);
        if (kids[i] == -1) err(EXIT_FAILURE, "clone(CLONE_VM)");
    }

    // Collect outcomes.
    int passed = 0, failed = 0;
    for (int i = 0; i < N_CHILDREN; i++) {
        char r;
        if (read(ready_pipe[0], &r, 1) != 1)
            err(EXIT_FAILURE, "read ready_pipe");
        if (r == 'P') passed++; else failed++;
    }

    // Release all children.
    for (int i = 0; i < N_CHILDREN; i++)
        write(release_pipe[1], "g", 1);

    for (int i = 0; i < N_CHILDREN; i++) {
        int ws;
        waitpid(kids[i], &ws, 0);
        free(stacks[i]);
    }

    printf("  INFO: %d/%d children's tcgetattr succeeded\n",
           passed, N_CHILDREN);

    // With shim: only SYNTH_OVERRIDE_SLOTS (=4) children can claim a
    // CoW slot; the rest see a stale real_fd and fail. Without shim:
    // there is no wrap to break, so all 8 succeed.
    CHECK(passed == N_CHILDREN,
          "all %d children should have succeeded", N_CHILDREN);

    close(master);
    close(ready_pipe[0]); close(ready_pipe[1]);
    close(release_pipe[0]); close(release_pipe[1]);

    TEST_REPORT();
}
