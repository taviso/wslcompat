/* Verify RLIMIT_CPU soft-limit delivery of SIGXCPU. Real Linux fires
 * SIGXCPU within ~1s of soft-limit-CPU-seconds being consumed. WSL1
 * silently never delivers. */
#define _GNU_SOURCE
#include <sys/resource.h>
#include <sys/time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

static volatile sig_atomic_t xcpu_fired;
static void on_xcpu(int s) { (void)s; xcpu_fired = 1; }

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

    struct sigaction alrm = { .sa_handler = timeout };
    sigemptyset(&alrm.sa_mask);
    sigaction(SIGALRM, &alrm, NULL);
    alarm(3);

    struct rlimit saved;
    if (getrlimit(RLIMIT_CPU, &saved) != 0)
        err(EXIT_FAILURE, "getrlimit");
    printf("RLIMIT_CPU initial: soft=%lu hard=%lu\n",
           (unsigned long)saved.rlim_cur, (unsigned long)saved.rlim_max);

    signal(SIGXCPU, on_xcpu);

    /* 1s soft limit is the smallest expressible. SIGXCPU should arrive
     * once the process accumulates ~1s of CPU. */
    struct rlimit rl = { .rlim_cur = 1, .rlim_max = saved.rlim_max };
    if (setrlimit(RLIMIT_CPU, &rl) != 0)
        err(EXIT_FAILURE, "setrlimit");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!xcpu_fired) {
        for (volatile int i = 0; i < 1000000; i++) { }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > 2500) break;
    }
    long ms = (t1.tv_sec - t0.tv_sec) * 1000
            + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    /* Restore before reporting in case the harness re-runs us. */
    setrlimit(RLIMIT_CPU, &saved);

    if (xcpu_fired) {
        printf("PASS: SIGXCPU fired after %ldms wall\n", ms);
        return 0;
    }
    printf("FAIL: SIGXCPU not delivered after %ldms of CPU burn\n", ms);
    return 1;
}
