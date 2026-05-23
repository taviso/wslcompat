/* Exploratory: does /proc/self/stat (and per-thread variant) provide
 * usable utime/stime fields on WSL1? This is the candidate backing store
 * for a CLOCK_PROCESS_CPUTIME_ID / CLOCK_THREAD_CPUTIME_ID polyfill. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <err.h>

static int failures;
static int subtests;
static long hz;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) {                                                     \
        printf("  PASS: " fmt "\n", ##__VA_ARGS__);                 \
    } else {                                                        \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                 \
        failures++;                                                 \
    }                                                               \
} while (0)

/* Parse the stat-format file pointed to by path, extract utime (field 14)
 * and stime (field 15) - in clock ticks. Field indexing per proc(5). */
static int read_cpu_ticks(const char *path, unsigned long long *utime,
                          unsigned long long *stime)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    /* The comm field is parenthesized and may contain spaces. Skip past
     * the closing ')' before tokenizing. */
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    p++;

    /* After ')' the next field is state (#3 in proc(5) numbering). We
     * need fields 14 and 15, so skip 14-3 = 11 whitespace-separated
     * tokens then read the next two. */
    int skip = 11;
    while (skip-- > 0) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    if (sscanf(p, " %llu %llu", utime, stime) != 2) return -1;
    return 0;
}

static void burn_cpu_ms(long want_ms)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        for (volatile int i = 0; i < 100000; i++) { }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= want_ms) break;
    }
}

static void test_proc_self_stat(void)
{
    printf("--- /proc/self/stat ---\n");
    unsigned long long u0, s0, u1, s1;

    if (read_cpu_ticks("/proc/self/stat", &u0, &s0) != 0) {
        printf("  FAIL: read+parse /proc/self/stat: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  before burn: utime=%llu stime=%llu (ticks)\n", u0, s0);

    burn_cpu_ms(200);

    if (read_cpu_ticks("/proc/self/stat", &u1, &s1) != 0) {
        printf("  FAIL: read+parse #2: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  after  burn: utime=%llu stime=%llu (ticks)\n", u1, s1);
    long long delta = (long long)(u1 + s1) - (long long)(u0 + s0);
    printf("  delta = %lld ticks (~%lld ms at HZ=%ld)\n",
           delta, delta * 1000 / hz, hz);

    /* Want at least ~150ms of accrued CPU after 200ms burn. */
    long min_ticks = 150 * hz / 1000;
    CHECK(delta >= min_ticks,
          "utime+stime advanced by %lld ticks (>= %ld required)",
          delta, min_ticks);
}

static void *thread_burner_main(void *arg)
{
    pid_t *tid_out = arg;
    *tid_out = (pid_t)syscall(SYS_gettid);

    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", *tid_out);

    unsigned long long u0, s0, u1, s1;
    if (read_cpu_ticks(path, &u0, &s0) != 0) {
        printf("  FAIL: read %s: %s\n", path, strerror(errno));
        failures++; subtests++;
        return NULL;
    }
    printf("  thread before: utime=%llu stime=%llu\n", u0, s0);

    burn_cpu_ms(200);

    if (read_cpu_ticks(path, &u1, &s1) != 0) {
        printf("  FAIL: read #2 %s: %s\n", path, strerror(errno));
        failures++; subtests++;
        return NULL;
    }
    printf("  thread after:  utime=%llu stime=%llu\n", u1, s1);
    long long delta = (long long)(u1 + s1) - (long long)(u0 + s0);
    printf("  delta = %lld ticks (~%lld ms)\n", delta, delta * 1000 / hz);

    long min_ticks = 150 * hz / 1000;
    CHECK(delta >= min_ticks,
          "thread utime+stime advanced by %lld ticks (>= %ld required)",
          delta, min_ticks);
    return NULL;
}

static void test_proc_task_stat(void)
{
    printf("--- /proc/self/task/<tid>/stat ---\n");
    pthread_t worker;
    pid_t tid = 0;
    int rc = pthread_create(&worker, NULL, thread_burner_main, &tid);
    if (rc != 0) {
        printf("  FAIL: pthread_create: %s\n", strerror(rc));
        failures++; subtests++;
        return;
    }
    pthread_join(worker, NULL);
    /* Did the per-thread directory exist and parse? Already accounted
     * for inside the thread function via CHECK. Add a separate sanity
     * check that the TID was a different value from getpid. */
    CHECK(tid != 0 && tid != getpid(),
          "thread tid=%d distinct from pid=%d", (int)tid, (int)getpid());
}

static void test_per_thread_isolation(void)
{
    /* Verify that one thread burning CPU does NOT bump another thread's
     * stat - i.e., the per-thread view is actually per-thread, not a
     * process-wide alias. */
    printf("--- per-thread isolation ---\n");
    pid_t my_tid = (pid_t)syscall(SYS_gettid);
    char my_path[64];
    snprintf(my_path, sizeof(my_path), "/proc/self/task/%d/stat", (int)my_tid);

    unsigned long long mu0, ms0, mu1, ms1;
    if (read_cpu_ticks(my_path, &mu0, &ms0) != 0) {
        printf("  FAIL: read %s: %s\n", my_path, strerror(errno));
        failures++; subtests++;
        return;
    }

    pthread_t worker;
    pid_t worker_tid = 0;
    pthread_create(&worker, NULL, thread_burner_main, &worker_tid);
    pthread_join(worker, NULL);

    if (read_cpu_ticks(my_path, &mu1, &ms1) != 0) {
        printf("  FAIL: read #2 %s: %s\n", my_path, strerror(errno));
        failures++; subtests++;
        return;
    }
    long long my_delta = (long long)(mu1 + ms1) - (long long)(mu0 + ms0);
    printf("  main thread delta while worker burned: %lld ticks\n", my_delta);
    /* Main thread was idle in pthread_join; should have accumulated near
     * zero ticks. Allow a small slack for scheduler overhead. */
    long max_ticks = 20 * hz / 1000; /* 20 ms */
    CHECK(my_delta <= max_ticks,
          "main thread delta %lld <= %ld (idle while worker burned)",
          my_delta, max_ticks);
}

static void timeout(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    hz = sysconf(_SC_CLK_TCK);
    printf("HZ = %ld\n", hz);

    struct sigaction sa = { .sa_handler = timeout };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(3);

    test_proc_self_stat();
    test_proc_task_stat();
    test_per_thread_isolation();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
