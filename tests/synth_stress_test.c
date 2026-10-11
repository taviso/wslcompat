/* Stress test for the synth-fd dispatch in libwslcompat.
 *
 * Race tests are inherently probabilistic, but hammering the
 * lifecycle from multiple threads for as long as the budget allows
 * gives us a reasonable shot at catching:
 *   - missed acquire/release barriers (torn reads of e->stat fields)
 *   - use-after-free of synth_entry during eviction
 *   - double-close of probe_fd
 *   - lost producer-ref drops -> entry never evicts
 *   - dispatch returning stale data while another thread is evicting
 *
 * Failure shapes:
 *   - crash / SIGSEGV (concurrency-driven memory corruption)
 *   - hang (alarm catches it)
 *   - wrong read content (torn struct, dispatched-to-wrong-entry)
 *   - table-full after a complete drain (eviction broken)
 *
 * Usage:
 *   ./synth_stress_test            # default 1 second total
 *   ./synth_stress_test 60         # 60 seconds for a manual soak
 *
 * On WSL1 we need the shim to satisfy these reads; the file doesn't
 * exist unshimmed -- this test is in TESTS. On real Linux the file
 * exists and the test exercises the kernel rather than our shim,
 * which is harmless: it just becomes a smoke test there. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NUM_THREADS  8
#define TABLE_CAP    64    /* matches SYNTH_MAX in synth.c */
#define EXTRA_PROBES 32

static const char *PATH = "/proc/sys/vm/mmap_min_addr";

/* Per-thread workers honour a shared monotonic deadline rather than
 * a fixed iteration count, so the test produces consistent suite
 * wall time across fast and slow boxes. */
static struct timespec g_deadline;

static void timeout_handler(int sig)
{
    (void) sig;
    static const char msg[] = "\nFAIL: stress test hung -- deadlock?\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static int past_deadline(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec != g_deadline.tv_sec)
        return now.tv_sec > g_deadline.tv_sec;
    return now.tv_nsec >= g_deadline.tv_nsec;
}

static void set_deadline_from_now(double seconds)
{
    clock_gettime(CLOCK_MONOTONIC, &g_deadline);
    long whole = (long) seconds;
    long nsec  = (long) ((seconds - whole) * 1e9);
    g_deadline.tv_sec  += whole;
    g_deadline.tv_nsec += nsec;
    if (g_deadline.tv_nsec >= 1000000000) {
        g_deadline.tv_sec  += 1;
        g_deadline.tv_nsec -= 1000000000;
    }
}

static ssize_t read_all(int fd, char *buf, size_t bufsize)
{
    size_t total = 0;
    while (total < bufsize - 1) {
        ssize_t n = read(fd, buf + total, bufsize - 1 - total);
        if (n < 0) return -1;
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* Validate the content looks like a /proc-style integer followed by
 * a newline. We don't pin to a specific value because the tunable
 * may have been set; we just want to detect torn / wrong / empty. */
static int looks_valid(const char *buf)
{
    if (buf[0] < '0' || buf[0] > '9')
        return 0;
    for (const char *p = buf; *p; p++) {
        if (*p == '\n')
            return 1;
        if (*p < '0' || *p > '9')
            return 0;
    }
    return 0;
}

static int run_thread_phase(const char *name, void *(*worker)(void *),
                            double seconds)
{
    pthread_t threads[NUM_THREADS];
    uintptr_t total_fail = 0;
    uintptr_t total_iter = 0;

    printf("--- %s (%d threads, budget %.2fs) ---\n",
           name, NUM_THREADS, seconds);

    set_deadline_from_now(seconds);

    for (long i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *) i) != 0) {
            printf("  FAIL: pthread_create: %s\n", strerror(errno));
            return 1;
        }
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        void *r;
        pthread_join(threads[i], &r);
        uintptr_t v = (uintptr_t) r;
        total_iter += v & 0xffffffff;
        total_fail += v >> 32;
    }
    printf("  %s: %lu iters, %lu errors\n",
           name, (unsigned long) total_iter, (unsigned long) total_fail);
    return total_fail == 0 ? 0 : 1;
}

/* Phase 1: pure dispatch contention. Many threads open/read/close
 * tightly, no shared fds. Stresses register, lookup, evict, cleanup
 * paths simultaneously across the table. */
static void *phase1_worker(void *arg)
{
    (void) arg;
    uintptr_t failures = 0;
    uintptr_t iters    = 0;
    while (!past_deadline()) {
        int fd = open(PATH, O_RDONLY);
        if (fd < 0) { failures++; continue; }

        char buf[64];
        ssize_t n = read_all(fd, buf, sizeof(buf));
        if (n <= 0 || !looks_valid(buf))
            failures++;

        if (close(fd) != 0)
            failures++;
        iters++;
    }
    return (void *) ((failures << 32) | (iters & 0xffffffff));
}

/* Phase 2: dup + dup3 onto a high fd + read through dup'd fd + close
 * both. Exercises the close intercept on multiple fds referencing the
 * same OFD AND the dup3 path so the probe-relocate has a chance to
 * trigger. */
static void *phase2_worker(void *arg)
{
    long tid = (long) arg;
    uintptr_t failures = 0;
    uintptr_t iters    = 0;
    int i = 0;
    while (!past_deadline()) {
        int fd = open(PATH, O_RDONLY);
        if (fd < 0) { failures++; i++; continue; }

        int d1 = dup(fd);
        if (d1 < 0) { close(fd); failures++; i++; continue; }

        int target = 200 + (int) tid * 32 + (i & 31);
        int d2 = dup3(fd, target, O_CLOEXEC);
        if (d2 < 0) {
            close(d1); close(fd); failures++; i++; continue;
        }

        char buf[64];
        ssize_t n = read_all(d2, buf, sizeof(buf));
        if (n <= 0 || !looks_valid(buf))
            failures++;

        if (close(d1) != 0) failures++;
        if (close(d2) != 0) failures++;
        if (close(fd) != 0) failures++;
        iters++;
        i++;
    }
    return (void *) ((failures << 32) | (iters & 0xffffffff));
}

/* Phase 3: fill the table, drain, refill. If eviction is broken, the
 * refill phase fails because slots stay occupied by ghost entries.
 * Single-threaded and bounded by table size, so it runs in a couple
 * of milliseconds and doesn't need a deadline. */
static int phase3_drain_and_refill(void)
{
    printf("--- Phase 3: drain and refill (single thread) ---\n");
    int held[TABLE_CAP + EXTRA_PROBES];
    int peak = 0;

    int stop_errno = 0;
    for (int i = 0; i < TABLE_CAP + EXTRA_PROBES; i++) {
        held[i] = open(PATH, O_RDONLY);
        if (held[i] < 0) { stop_errno = errno; break; }
        peak++;
    }
    printf("  Phase 3: peak open count before refusal = %d (errno=%s)\n",
           peak, stop_errno ? strerror(stop_errno) : "n/a");

    if (peak < TABLE_CAP) {
        printf("  FAIL: expected at least %d entries before refusal\n",
               TABLE_CAP);
        for (int i = 0; i < peak; i++) close(held[i]);
        return 1;
    }

    for (int i = 0; i < peak; i++)
        close(held[i]);

    int refill = 0;
    for (int i = 0; i < TABLE_CAP; i++) {
        held[i] = open(PATH, O_RDONLY);
        if (held[i] < 0)
            break;
        refill++;
    }
    printf("  Phase 3: refilled %d entries after drain (want %d)\n",
           refill, TABLE_CAP);

    int ok = (refill >= TABLE_CAP);
    for (int i = 0; i < refill; i++) close(held[i]);
    return ok ? 0 : 1;
}

/* Phase 4: Relocation Collision Storm. Every thread attempts to dup3
 * onto the same target FD simultaneously. Stresses the internal
 * relocate_probe CAS loop. */
static void *phase4_worker(void *arg)
{
    (void) arg;
    uintptr_t failures = 0;
    uintptr_t iters    = 0;
    const int TARGET = 100;

    while (!past_deadline()) {
        int fd = open(PATH, O_RDONLY);
        if (fd < 0) { failures++; continue; }

        /* Every thread races to occupy FD 100. */
        int d1 = dup3(fd, TARGET, O_CLOEXEC);
        if (d1 < 0) {
            /* Collision expected, but if it fails for reasons other
             * than the target being busy (which should be handled by 
             * our shim relocating the previous owner), it's a bug. */
            if (errno != EBUSY && errno != EBADF) failures++;
        } else {
            char buf[64];
            ssize_t n = read_all(d1, buf, sizeof(buf));
            /* Because multiple threads might be dupping onto TARGET,
             * it might be closed or hit EOF mid-read. */
            if (n > 0 && !looks_valid(buf))
                failures++;
            close(d1);
        }
        close(fd);
        iters++;
    }
    return (void *) ((failures << 32) | (iters & 0xffffffff));
}

/* Phase 5: OFD Sharing & Delayed Eviction. Verifies that an entry
 * stays alive as long as ANY fd (including dups/forks) holds the OFD. */
static int phase5_ofd_sharing(void)
{
    printf("--- Phase 5: OFD sharing & delayed eviction ---\n");
    int fd = open(PATH, O_RDONLY);
    if (fd < 0) return 1;

    int dups[10];
    for (int i = 0; i < 10; i++) {
        dups[i] = dup(fd);
        if (dups[i] < 0) {
            for (int j = 0; j < i; j++) close(dups[j]);
            close(fd);
            return 1;
        }
    }

    /* Close the original and most dups. */
    close(fd);
    for (int i = 0; i < 9; i++) close(dups[i]);

    /* The last dup should still be fully functional. */
    char buf[64];
    if (read_all(dups[9], buf, sizeof(buf)) <= 0 || !looks_valid(buf)) {
        printf("  FAIL: entry evicted while one dup still alive\n");
        close(dups[9]);
        return 1;
    }

    close(dups[9]);
    printf("  Phase 5: PASS\n");
    return 0;
}

/* Phase 6: Shared FD Dispatch Storm. One FD, many threads hammering
 * read() to stress the refcount of a single entry. */
static int g_shared_fd = -1;

static void *phase6_worker(void *arg)
{
    (void) arg;
    uintptr_t failures = 0;
    uintptr_t iters    = 0;
    char buf[64];

    while (!past_deadline()) {
        ssize_t n = read_all(g_shared_fd, buf, sizeof(buf));
        if (n > 0 && !looks_valid(buf))
            failures++;
        iters++;
    }
    return (void *) ((failures << 32) | (iters & 0xffffffff));
}

static int run_shared_storm(void)
{
    g_shared_fd = open(PATH, O_RDONLY);
    if (g_shared_fd < 0) return 1;

    int r = run_thread_phase("Phase 6: shared FD dispatch storm",
                             phase6_worker, 1.0);
    close(g_shared_fd);
    return r;
}

/* Phase 7: Mixed FD Interop Storm. Exhaustively tests dup2/dup3
 * between regular and synthetic FDs to verify the implicit-close
 * logic in the shims. */
static void *phase7_worker(void *arg)
{
    (void) arg;
    uintptr_t failures = 0;
    uintptr_t iters    = 0;
    const char *REG_PATH = "/etc/passwd";

    while (!past_deadline()) {
        int s1 = open(PATH, O_RDONLY);
        int r1 = open(REG_PATH, O_RDONLY);
        if (s1 < 0 || r1 < 0) {
            if (s1 >= 0) close(s1);
            if (r1 >= 0) close(r1);
            failures++;
            continue;
        }

        /* Case 1: regular onto synthetic (should evict s1) */
        if (dup2(r1, s1) < 0) failures++;
        
        /* Case 2: synthetic onto regular (should overwrite r1, s1's entry stays) */
        int s2 = open(PATH, O_RDONLY);
        if (s2 >= 0) {
            if (dup3(s2, r1, O_CLOEXEC) < 0) failures++;
            
            /* Verify it works. */
            char buf[64];
            if (read_all(r1, buf, sizeof(buf)) <= 0 || !looks_valid(buf))
                failures++;
            close(s2);
        } else {
            failures++;
        }

        /* Case 3: self-dup (no-op) */
        if (dup2(s1, s1) < 0) failures++;

        close(s1);
        close(r1);
        iters++;
    }
    return (void *) ((failures << 32) | (iters & 0xffffffff));
}

int main(int argc, char **argv)
{
    double total_seconds = 1.0;
    int failures = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1) {
        total_seconds = atof(argv[1]);
        if (total_seconds <= 0) {
            fprintf(stderr, "usage: %s [seconds]\n", argv[0]);
            return 2;
        }
    }

    /* Split the time budget across the timed phases. */
    double per_phase = total_seconds / 4.0;

    /* Watchdog with comfortable slack -- if we hang past this the
     * test almost certainly hit a deadlock. */
    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm((unsigned int) (total_seconds + 30));

    failures += run_thread_phase("Phase 1: open/read/close storm",
                                 phase1_worker, per_phase);
    failures += run_thread_phase("Phase 2: open/dup/dup3/read/close storm",
                                 phase2_worker, per_phase);
    failures += phase3_drain_and_refill();
    failures += run_thread_phase("Phase 4: relocation collision storm",
                                 phase4_worker, per_phase);
    failures += phase5_ofd_sharing();
    failures += run_shared_storm();
    failures += run_thread_phase("Phase 7: mixed FD interop storm",
                                 phase7_worker, per_phase);

    if (failures > 0) {
        printf("FAIL: %d phase(s) failed\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
