/* Verify the kernel enforces RLIMIT_CPU's hard limit by killing the
 * offender with SIGKILL when the limit is reached. Real Linux delivers
 * SIGKILL within ~1s of CPU burn after setting hard=1s. Run in a
 * forked child so a positive result doesn't kill the test harness. */
#define _GNU_SOURCE
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

static void timeout(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: test timed out (waitpid never returned)\n";
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

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        /* Child: install soft=hard=1s, burn CPU, expect SIGKILL. If we
         * make it past ~2.5s of wall time without dying, the kernel
         * isn't enforcing the limit - exit with a sentinel so the
         * parent can distinguish. */
        struct rlimit rl = { .rlim_cur = 1, .rlim_max = 1 };
        if (setrlimit(RLIMIT_CPU, &rl) != 0) {
            fprintf(stderr, "child: setrlimit: %s\n", strerror(errno));
            _exit(2);
        }
        /* Ignore SIGXCPU so the soft-limit path doesn't tear us down
         * before we test the hard-limit path. */
        signal(SIGXCPU, SIG_IGN);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            for (volatile int i = 0; i < 1000000; i++) { }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long ms = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (ms > 2500) _exit(3); /* survived past the limit */
        }
    }

    /* Parent */
    int status;
    if (waitpid(pid, &status, 0) < 0)
        err(EXIT_FAILURE, "waitpid");

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGKILL) {
            printf("PASS: child killed by SIGKILL (hard RLIMIT_CPU enforced)\n");
            return 0;
        }
        printf("FAIL: child killed by signal %d (expected SIGKILL)\n", sig);
        return 1;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 3) {
            printf("FAIL: child survived past hard limit (kernel did not enforce)\n");
        } else {
            printf("FAIL: child exited with code %d\n", code);
        }
        return 1;
    }
    printf("FAIL: unexpected wait status 0x%x\n", status);
    return 1;
}
