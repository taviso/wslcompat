/* Verify the kernel can deliver a signal driven by process/thread CPU
 * time. Real Linux supports all four mechanisms below; WSL1 supports
 * none of them. If any of these become available later (real impl or
 * future polyfill), the test will start passing. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) {                                                     \
        printf("  PASS: " fmt "\n", ##__VA_ARGS__);                 \
    } else {                                                        \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                 \
        failures++;                                                 \
    }                                                               \
} while (0)

static volatile sig_atomic_t signal_fired;
static void on_signal(int s) { (void)s; signal_fired = 1; }

/* Burn CPU until *flag is set or wall-clock cap reached. Returns
 * wall-clock ms elapsed. */
static long burn_until(volatile sig_atomic_t *flag, long cap_ms)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        for (volatile int i = 0; i < 100000; i++) { }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (*flag || ms >= cap_ms) return ms;
    }
}

static void test_itimer(int which, int sig, const char *name)
{
    printf("--- %s ---\n", name);
    signal(sig, on_signal);
    struct itimerval it = { {0,0}, {0, 50000} }; /* 50ms */
    signal_fired = 0;
    if (setitimer(which, &it, NULL) != 0) {
        printf("  FAIL: setitimer(%s): %s\n", name, strerror(errno));
        failures++; subtests++;
        return;
    }
    long ms = burn_until(&signal_fired, 500);
    /* Cancel the timer so it doesn't fire again later. */
    struct itimerval off = {{0,0},{0,0}};
    setitimer(which, &off, NULL);
    CHECK(signal_fired,
          "%s fired after %ldms wall (signal=%d)", name, ms, sig);
}

static void test_timer_create(clockid_t clk, const char *name)
{
    printf("--- timer_create(%s) ---\n", name);
    timer_t tid;
    struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL,
                            .sigev_signo  = SIGUSR1 };
    if (timer_create(clk, &sev, &tid) != 0) {
        printf("  FAIL: timer_create(%s): %s\n", name, strerror(errno));
        failures++; subtests++;
        return;
    }
    CHECK(1, "timer_create(%s) succeeded", name);
    timer_delete(tid);
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

    struct sigaction sa = { .sa_handler = timeout };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(3);

    test_itimer(ITIMER_PROF,    SIGPROF,   "ITIMER_PROF");
    test_itimer(ITIMER_VIRTUAL, SIGVTALRM, "ITIMER_VIRTUAL");
    test_timer_create(CLOCK_PROCESS_CPUTIME_ID, "CLOCK_PROCESS_CPUTIME_ID");
    test_timer_create(CLOCK_THREAD_CPUTIME_ID,  "CLOCK_THREAD_CPUTIME_ID");

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
