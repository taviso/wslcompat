/* Canary for the override-slot acquire race in synth.c.
 *
 * wslcompat_override_acquire_pid() does a two-pass "find my slot, else
 * claim a free slot" walk. Two threads of the same process (same
 * pid+starttime identity) can both miss pass 1 and claim two different
 * slots, orphaning one and leaking its dup'd fd.
 *
 * Reachability is subtle: the racer must be a non-installer process
 * (fork/vfork child) whose threads concurrently trigger relocate on the
 * same entry. A fork child gets a COW copy of the slot array, so every
 * child is an independent trial.
 *
 * The shim's relocate path is load -> F_DUPFD_CLOEXEC -> store. The
 * dup syscall delays publication, so N threads closing the same internal
 * fd all pass the load check and converge on acquire together.
 *
 * Detection: each correct relocate is net-zero (close internal_fd, gain
 * one relocated dup). A racer that claims a second slot leaks its dup,
 * so the child's fd count rises. A rising count means the race fired.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <err.h>

#include "testutil.h"

#define DEFAULT_THREADS 8
#define DEFAULT_TRIALS  256

static int g_internal_fd;
static atomic_int g_go;

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

static int snapshot_fds(int *out, size_t cap)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) && (size_t)n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        int fd = atoi(e->d_name);
        if (fd == dirfd(d))
            continue;
        out[n++] = fd;
    }
    closedir(d);
    return n;
}

/* Find an fd number present in the after-snapshot but not before. */
static int find_new_fd(const int *before, int n_before,
                       const int *after, int n_after)
{
    for (int j = 0; j < n_after; j++) {
        int found = 0;
        for (int i = 0; i < n_before; i++)
            if (before[i] == after[j]) { found = 1; break; }
        if (!found)
            return after[j];
    }
    return -1;
}

static void *storm_worker(void *arg)
{
    (void) arg;
    while (!atomic_load_explicit(&g_go, memory_order_acquire))
        ;
    close(g_internal_fd);
    return NULL;
}

/* One trial: N threads race to close internal_fd, then measure the
 * fd-count delta. Returns the delta (0 == clean, >0 == leak). */
static int run_trial(int nthreads)
{
    int before = wslcompat_test_count_fds();
    if (before < 0) err(EXIT_FAILURE, "count_fds");

    atomic_store(&g_go, 0);
    pthread_t *threads = calloc(nthreads, sizeof(*threads));
    if (!threads) err(EXIT_FAILURE, "calloc");

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, storm_worker, NULL) != 0)
            err(EXIT_FAILURE, "pthread_create");
    }

    atomic_store_explicit(&g_go, 1, memory_order_release);

    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);
    free(threads);

    int after = wslcompat_test_count_fds();
    if (after < 0) err(EXIT_FAILURE, "count_fds");
    return after - before;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);

    int nthreads = DEFAULT_THREADS;
    int trials   = DEFAULT_TRIALS;
    if (argc > 1) nthreads = atoi(argv[1]);
    if (argc > 2) trials   = atoi(argv[2]);

    int master, slave;
    open_pty(&master, &slave);

    int before[256], after[256];
    int n_before = snapshot_fds(before, 256);

    struct termios t;
    if (tcgetattr(slave, &t) != 0) err(EXIT_FAILURE, "tcgetattr");
    t.c_lflag &= ~ICANON;
    if (tcsetattr(slave, TCSANOW, &t) != 0) err(EXIT_FAILURE, "tcsetattr");

    int n_after = snapshot_fds(after, 256);
    g_internal_fd = find_new_fd(before, n_before, after, n_after);

    if (g_internal_fd < 0) {
        printf("SKIP: no internal fd detected (unshimmed run)\n");
        return 0;
    }
    printf("INFO: internal_fd=%d, %d threads, %d trials\n",
           g_internal_fd, nthreads, trials);

    int leaks = 0;
    for (int i = 0; i < trials; i++) {
        pid_t pid = fork();
        if (pid < 0) err(EXIT_FAILURE, "fork");
        if (pid == 0) {
            int delta = run_trial(nthreads);
            _exit(delta > 0 ? 1 : 0);
        }
        int ws;
        if (waitpid(pid, &ws, 0) < 0) err(EXIT_FAILURE, "waitpid");
        if (WIFEXITED(ws) && WEXITSTATUS(ws) == 1)
            leaks++;
    }

    close(master);
    close(slave);

    printf("INFO: %d/%d trials detected a leaked fd\n", leaks, trials);

    CHECK(leaks == 0, "no override-slot acquire race observed (%d trials)", trials);
    TEST_REPORT();
}
