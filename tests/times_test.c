/* Verify times(2) and getrusage(2) return usable CPU-time counters.
 * Both are candidate backings for a CLOCK_PROCESS_CPUTIME_ID polyfill;
 * getrusage(RUSAGE_THREAD) additionally covers CLOCK_THREAD_CPUTIME_ID
 * if the kernel supports it. */
#define _GNU_SOURCE
#include <sys/times.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <valgrind/valgrind.h>
#include <valgrind/valgrind.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static void burn_ms(long want_ms)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        for (volatile int i = 0; i < 100000; i++) { }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= want_ms) return;
    }
}

static long hz;

static long tv_to_us(const struct timeval *tv)
{
    return tv->tv_sec * 1000000L + tv->tv_usec;
}

static void test_times(void)
{
    printf("--- times(2) ---\n");
    struct tms t0, t1;
    clock_t r0 = times(&t0);
    if (r0 == (clock_t)-1) {
        printf("  FAIL: times(): %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  before: utime=%ld stime=%ld cutime=%ld cstime=%ld (return=%ld)\n",
           (long)t0.tms_utime, (long)t0.tms_stime,
           (long)t0.tms_cutime, (long)t0.tms_cstime, (long)r0);

    burn_ms(200);

    clock_t r1 = times(&t1);
    if (r1 == (clock_t)-1) {
        printf("  FAIL: times() #2: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  after:  utime=%ld stime=%ld cutime=%ld cstime=%ld (return=%ld)\n",
           (long)t1.tms_utime, (long)t1.tms_stime,
           (long)t1.tms_cutime, (long)t1.tms_cstime, (long)r1);

    long cpu_delta = (long)(t1.tms_utime + t1.tms_stime)
                   - (long)(t0.tms_utime + t0.tms_stime);
    long wall_delta = (long)(r1 - r0);
    long min_ticks = 150 * hz / 1000;
    CHECK(cpu_delta >= min_ticks,
          "utime+stime advanced by %ld ticks (~%ldms; >= %ld required)",
          cpu_delta, cpu_delta * 1000 / hz, min_ticks);
    CHECK(wall_delta >= min_ticks,
          "return-value advanced by %ld ticks (~%ldms; >= %ld)",
          wall_delta, wall_delta * 1000 / hz, min_ticks);
}

static void test_getrusage_self(void)
{
    printf("--- getrusage(RUSAGE_SELF) ---\n");
    struct rusage r0, r1;
    if (getrusage(RUSAGE_SELF, &r0) != 0) {
        printf("  FAIL: getrusage(RUSAGE_SELF): %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  before: utime=%ld.%06ld stime=%ld.%06ld\n",
           (long)r0.ru_utime.tv_sec, (long)r0.ru_utime.tv_usec,
           (long)r0.ru_stime.tv_sec, (long)r0.ru_stime.tv_usec);

    burn_ms(200);

    if (getrusage(RUSAGE_SELF, &r1) != 0) {
        printf("  FAIL: getrusage #2: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  after:  utime=%ld.%06ld stime=%ld.%06ld\n",
           (long)r1.ru_utime.tv_sec, (long)r1.ru_utime.tv_usec,
           (long)r1.ru_stime.tv_sec, (long)r1.ru_stime.tv_usec);

    long delta_us = (tv_to_us(&r1.ru_utime) + tv_to_us(&r1.ru_stime))
                  - (tv_to_us(&r0.ru_utime) + tv_to_us(&r0.ru_stime));
    CHECK(delta_us >= 150000,
          "RUSAGE_SELF utime+stime advanced by %ldus (>= 150000)", delta_us);
}

static void *thread_burner(void *arg)
{
    struct rusage *out = arg;
    burn_ms(200);
    if (getrusage(RUSAGE_THREAD, out) != 0) {
        out->ru_utime.tv_sec  = -1;
        out->ru_utime.tv_usec = errno;
    }
    return NULL;
}

static void test_getrusage_thread(void)
{
    /* RUSAGE_THREAD is a Linux extension; absence is itself a finding. */
    printf("--- getrusage(RUSAGE_THREAD) ---\n");
    struct rusage main0;
    if (getrusage(RUSAGE_THREAD, &main0) != 0) {
        printf("  FAIL: getrusage(RUSAGE_THREAD): %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    printf("  main before: utime=%ld.%06ld stime=%ld.%06ld\n",
           (long)main0.ru_utime.tv_sec, (long)main0.ru_utime.tv_usec,
           (long)main0.ru_stime.tv_sec, (long)main0.ru_stime.tv_usec);

    struct rusage worker_after = {0};
    pthread_t worker;
    if (pthread_create(&worker, NULL, thread_burner, &worker_after) != 0) {
        printf("  FAIL: pthread_create\n");
        failures++; subtests++;
        return;
    }
    pthread_join(worker, NULL);

    if (worker_after.ru_utime.tv_sec == -1) {
        printf("  FAIL: worker getrusage(RUSAGE_THREAD): %s\n",
               strerror((int)worker_after.ru_utime.tv_usec));
        failures++; subtests++;
        return;
    }
    printf("  worker after burn: utime=%ld.%06ld stime=%ld.%06ld\n",
           (long)worker_after.ru_utime.tv_sec, (long)worker_after.ru_utime.tv_usec,
           (long)worker_after.ru_stime.tv_sec, (long)worker_after.ru_stime.tv_usec);

    long worker_us = tv_to_us(&worker_after.ru_utime)
                   + tv_to_us(&worker_after.ru_stime);
    CHECK(worker_us >= 150000,
          "worker thread CPU = %ldus (>= 150000)", worker_us);

    /* Main thread was idle in pthread_join; should not have accumulated
     * meaningful CPU. */
    struct rusage main1;
    if (getrusage(RUSAGE_THREAD, &main1) != 0) {
        printf("  FAIL: main getrusage #2: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    long main_delta_us = (tv_to_us(&main1.ru_utime) + tv_to_us(&main1.ru_stime))
                       - (tv_to_us(&main0.ru_utime) + tv_to_us(&main0.ru_stime));
    printf("  main delta while worker burned: %ldus\n", main_delta_us);
    /* Valgrind serializes threads and deposits a couple of Windows
     * ticks (~15.6ms each) on the parent during pthread_join - flaky
     * in practice. Memory safety is what valgrind cares about; skip
     * this timing assertion under it. */
    if (RUNNING_ON_VALGRIND) {
        printf("  SKIP: main thread idle check under valgrind\n");
    } else {
        CHECK(main_delta_us <= 30000,
              "main thread delta %ldus <= 30000 (idle while worker burned)",
              main_delta_us);
    }
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

    test_times();
    test_getrusage_self();
    test_getrusage_thread();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
