#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "testutil.h"

#define TRIALS  80
#define POLL_TIMEOUT_MS 100

static volatile sig_atomic_t sig_count;
static atomic_int            flooder_stop;
static struct timespec       kill_ts;

static void on_alrm(int s) {
    (void)s;
    clock_gettime(CLOCK_MONOTONIC, &kill_ts);
    sig_count++;
}

// tcsetattr on the slave triggers tty_install under the shim and
// creates a synth wrap. On native Linux it's just a no-op tcsetattr.
static int wrap_pty(int *master, int *slave)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    if (grantpt(m) < 0 || unlockpt(m) < 0) { close(m); return -1; }
    char *name = ptsname(m);
    if (name == NULL) { close(m); return -1; }
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s < 0) { close(m); return -1; }
    struct termios t;
    if (tcgetattr(s, &t) < 0) { close(m); close(s); return -1; }
    if (tcsetattr(s, TCSANOW, &t) < 0) { close(m); close(s); return -1; }
    *master = m;
    *slave  = s;
    return 0;
}

static void *flooder_fn(void *arg)
{
    (void)arg;
    while (!atomic_load(&flooder_stop)) {
        int cm, cs;
        if (wrap_pty(&cm, &cs) < 0)
            break;
        close(cm);
        close(cs);
    }
    return NULL;
}

// Run TRIALS rounds: for each, fork a killer that sleeps 2ms then
// kills(self, SIGUSR1), and meanwhile poll a pipe with POLL_TIMEOUT_MS.
// Returns the number of trials where the handler ran but poll did not
// return EINTR (i.e. lost-EINTR events).
static int run_trials(const char *label, int pipefd)
{
    pid_t self = getpid();
    int eintr_count = 0, no_signal = 0, missed_eintr = 0;
    long max_missed_elapsed = 0;

    for (int t = 0; t < TRIALS; t++) {
        int before = sig_count;
        kill_ts.tv_sec = 0;
        kill_ts.tv_nsec = 0;

        struct itimerval it = {0};
        it.it_value.tv_usec = 20000;  // 20ms
        setitimer(ITIMER_REAL, &it, NULL);

        struct pollfd pfd = { .fd = pipefd, .events = POLLIN };
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = poll(&pfd, 1, POLL_TIMEOUT_MS);
        int saved = errno;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        // Cancel timer in case it hasn't fired
        struct itimerval cancel = {0};
        setitimer(ITIMER_REAL, &cancel, NULL);
        long elapsed = (t1.tv_sec - t0.tv_sec) * 1000
                     + (t1.tv_nsec - t0.tv_nsec) / 1000000;

        long kill_ms = kill_ts.tv_sec * 1000 + kill_ts.tv_nsec / 1000000;
        long t0_ms = t0.tv_sec * 1000 + t0.tv_nsec / 1000000;
        long t1_ms = t1.tv_sec * 1000 + t1.tv_nsec / 1000000;

        // sig_count not advancing means the SIGALRM hadn't
        // landed by the time poll returned
        int delivered = (sig_count > before);
        
        // If the signal arrived before poll even started, or after it returned,
        // we can't definitively say poll swallowed it.
        int sent_early = (kill_ms > 0 && kill_ms <= t0_ms);
        int sent_late = (kill_ms > 0 && kill_ms >= t1_ms);

        if (r == -1 && saved == EINTR) {
            eintr_count++;
        } else if (!delivered || sent_early || sent_late) {
            no_signal++;
        } else {
            missed_eintr++;
            if (elapsed > max_missed_elapsed)
                max_missed_elapsed = elapsed;
        }
    }

    STATUS("[%s] trials=%d eintr=%d no_signal=%d missed_eintr=%d max_missed=%ldms",
           label, TRIALS, eintr_count, no_signal, missed_eintr, max_missed_elapsed);
    return missed_eintr;
}

int main(void)
{
    alarm(30);

    struct sigaction sa = { .sa_handler = on_alrm };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART -- we want EINTR
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        fprintf(stderr, "sigaction: %m\n");
        return 70;
    }

    int pipefds[2];
    if (pipe(pipefds) < 0) {
        fprintf(stderr, "pipe: %m\n");
        return 70;
    }

    // ----- quiet-bare: no wraps anywhere, poll must always EINTR -----
    int n = run_trials("quiet-bare", pipefds[0]);
    CHECK(n >= 0, "[quiet-bare] test executed successfully");
    CHECK(n == 0, "[quiet-bare] zero-overhead path missed %d EINTR", n);

    // ----- quiet-hold: wraps held quietly, no closes, must still EINTR -----
    enum { HELD = 8 };
    int held_m[HELD], held_s[HELD];
    for (int i = 0; i < HELD; i++) {
        if (wrap_pty(&held_m[i], &held_s[i]) < 0) {
            fprintf(stderr, "wrap_pty held[%d]: %m\n", i);
            return 70;
        }
    }
    n = run_trials("quiet-hold", pipefds[0]);
    CHECK(n >= 0, "[quiet-hold] test executed successfully");
    CHECK(n == 0, "[quiet-hold] wraps held quietly missed %d EINTR", n);

    // ----- churn: flooder closing wraps continuously, EINTR can be lost -----
    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block, &prev);

    pthread_t flooder;
    atomic_store(&flooder_stop, 0);
    if (pthread_create(&flooder, NULL, flooder_fn, NULL) != 0) {
        fprintf(stderr, "pthread_create: %m\n");
        return 70;
    }

    pthread_sigmask(SIG_SETMASK, &prev, NULL);

    n = run_trials("churn", pipefds[0]);

    atomic_store(&flooder_stop, 1);
    pthread_join(flooder, NULL);

    for (int i = 0; i < HELD; i++) { close(held_m[i]); close(held_s[i]); }
    close(pipefds[0]);
    close(pipefds[1]);

    CHECK(n >= 0, "[churn] test executed successfully");
    CHECK(n == 0, "[churn] every delivered SIGALRM produced EINTR even under churn (missed %d)", n);

    TEST_REPORT();
}
