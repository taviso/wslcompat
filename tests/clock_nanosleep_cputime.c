#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>

/* Per-test alarm: SIGALRM handler just sets a flag so blocking calls
 * unwind with EINTR (no SA_RESTART) and the test continues instead of
 * hanging. Global cap is enforced too. */
static volatile sig_atomic_t alarm_fired;
static void alarm_handler(int sig) { (void)sig; alarm_fired = 1; }

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

struct clock_entry {
    clockid_t id;
    const char *name;
    int nanosleep_supported; /* 1 if kernel accepts this in clock_nanosleep */
};

/* Per Linux man clock_nanosleep(2): supported clocks are
 *   CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME,
 *   CLOCK_PROCESS_CPUTIME_ID, CLOCK_TAI.
 * CLOCK_THREAD_CPUTIME_ID is explicitly not allowed. */
static struct clock_entry clocks[] = {
    { CLOCK_REALTIME,           "CLOCK_REALTIME",           1 },
    { CLOCK_MONOTONIC,          "CLOCK_MONOTONIC",          1 },
    { CLOCK_PROCESS_CPUTIME_ID, "CLOCK_PROCESS_CPUTIME_ID", 1 },
    { CLOCK_THREAD_CPUTIME_ID,  "CLOCK_THREAD_CPUTIME_ID",  0 },
    { CLOCK_MONOTONIC_RAW,      "CLOCK_MONOTONIC_RAW",      0 },
    { CLOCK_REALTIME_COARSE,    "CLOCK_REALTIME_COARSE",    0 },
    { CLOCK_MONOTONIC_COARSE,   "CLOCK_MONOTONIC_COARSE",   0 },
    { CLOCK_BOOTTIME,           "CLOCK_BOOTTIME",           1 },
    { CLOCK_TAI,                "CLOCK_TAI",                1 },
};

static int valid_timespec(const struct timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L;
}

static void test_getres_gettime(struct clock_entry *c)
{
    struct timespec res, now;

    if (clock_getres(c->id, &res) != 0) {
        printf("  FAIL: %s: clock_getres: %s\n", c->name, strerror(errno));
        failures++; subtests++;
        return;
    }
    subtests++;
    CHECK(valid_timespec(&res),
          "%s: clock_getres = %ld.%09ld",
          c->name, (long)res.tv_sec, res.tv_nsec);

    if (clock_gettime(c->id, &now) != 0) {
        printf("  FAIL: %s: clock_gettime: %s\n", c->name, strerror(errno));
        failures++; subtests++;
        return;
    }
    subtests++;
    CHECK(valid_timespec(&now) && (now.tv_sec != 0 || now.tv_nsec != 0),
          "%s: clock_gettime = %ld.%09ld",
          c->name, (long)now.tv_sec, now.tv_nsec);
}

static void test_nanosleep_abstime_past(struct clock_entry *c)
{
    struct timespec past = { 0, 0 };
    alarm_fired = 0;
    alarm(1);
    int rc = clock_nanosleep(c->id, TIMER_ABSTIME, &past, NULL);
    alarm(0);

    if (c->nanosleep_supported) {
        CHECK(rc == 0,
              "%s: nanosleep(ABSTIME, past) -> %d (%s)",
              c->name, rc, rc ? strerror(rc) : "ok");
    } else {
        CHECK(rc == EINVAL || rc == ENOTSUP,
              "%s: nanosleep(ABSTIME, past) rejected with %d (%s)",
              c->name, rc, strerror(rc));
    }
}

static void test_nanosleep_relative_short(void)
{
    /* 1ms relative sleep on CLOCK_MONOTONIC should return 0 well under
     * our per-test alarm. */
    struct timespec req = { 0, 1000000L }; /* 1 ms */
    alarm_fired = 0;
    alarm(1);
    int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
    alarm(0);
    CHECK(rc == 0,
          "CLOCK_MONOTONIC: 1ms relative nanosleep -> %d (%s)",
          rc, rc ? strerror(rc) : "ok");
}

static void test_nanosleep_abstime_future(void)
{
    /* MONOTONIC + 5ms absolute: should block briefly then return 0. */
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        printf("  FAIL: nanosleep_abstime_future: gettime: %s\n",
               strerror(errno));
        failures++; subtests++;
        return;
    }
    t.tv_nsec += 5000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }

    alarm_fired = 0;
    alarm(1);
    int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    alarm(0);

    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &after);
    long long delta_ns = (long long)(after.tv_sec - t.tv_sec) * 1000000000LL
                       + (after.tv_nsec - t.tv_nsec);
    CHECK(rc == 0 && delta_ns >= 0,
          "CLOCK_MONOTONIC: ABSTIME +5ms -> %d, overshoot=%lldns",
          rc, delta_ns);
}

static void test_getcpuclockid_self(void)
{
    clockid_t cid;
    int rc = clock_getcpuclockid(0, &cid);
    if (rc != 0) {
        printf("  FAIL: clock_getcpuclockid(0): %s\n", strerror(rc));
        failures++; subtests++;
        return;
    }
    subtests++;
    printf("  PASS: clock_getcpuclockid(0) -> 0x%x\n", (unsigned)cid);

    struct timespec ts;
    int rc2 = clock_gettime(cid, &ts);
    CHECK(rc2 == 0 && valid_timespec(&ts),
          "clock_gettime(self cpuclockid) -> rc=%d ts=%ld.%09ld",
          rc2, (long)ts.tv_sec, ts.tv_nsec);
}

static void test_getcpuclockid_getpid(void)
{
    clockid_t cid;
    int rc = clock_getcpuclockid(getpid(), &cid);
    if (rc != 0) {
        printf("  FAIL: clock_getcpuclockid(getpid()): %s\n", strerror(rc));
        failures++; subtests++;
        return;
    }
    subtests++;
    printf("  PASS: clock_getcpuclockid(getpid()) -> 0x%x\n", (unsigned)cid);

    struct timespec ts;
    int rc2 = clock_gettime(cid, &ts);
    CHECK(rc2 == 0 && valid_timespec(&ts),
          "clock_gettime(getpid cpuclockid) -> rc=%d ts=%ld.%09ld",
          rc2, (long)ts.tv_sec, ts.tv_nsec);
}

static void test_pthread_getcpuclockid(void)
{
    clockid_t cid;
    int rc = pthread_getcpuclockid(pthread_self(), &cid);
    if (rc != 0) {
        printf("  FAIL: pthread_getcpuclockid: %s\n", strerror(rc));
        failures++; subtests++;
        return;
    }
    subtests++;
    printf("  PASS: pthread_getcpuclockid(self) -> 0x%x\n", (unsigned)cid);

    struct timespec ts;
    int rc2 = clock_gettime(cid, &ts);
    CHECK(rc2 == 0 && valid_timespec(&ts),
          "clock_gettime(thread cpuclockid) -> rc=%d ts=%ld.%09ld",
          rc2, (long)ts.tv_sec, ts.tv_nsec);
}

static void test_invalid_clockid(void)
{
    /* A bogus clock id should be rejected. Linux returns EINVAL. */
    struct timespec ts;
    int rc = clock_gettime((clockid_t)0xdeadbeef, &ts);
    CHECK(rc == -1 && errno == EINVAL,
          "clock_gettime(bogus) -> rc=%d errno=%d", rc, errno);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = { .sa_handler = alarm_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: let blocking calls return EINTR */
    sigaction(SIGALRM, &sa, NULL);

    /* CPU clocks have a 15.625ms resolution on WSL1 (Windows scheduler
     * tick). A fresh process may not have accumulated even one tick yet,
     * which would make our "non-zero" gettime check fail spuriously.
     * Burn ~25ms wall to guarantee a tick or two of CPU before sampling. */
    struct timespec wall_t0, wall_t1;
    clock_gettime(CLOCK_MONOTONIC, &wall_t0);
    for (;;) {
        for (volatile int i = 0; i < 100000; i++) { }
        clock_gettime(CLOCK_MONOTONIC, &wall_t1);
        long ms = (wall_t1.tv_sec - wall_t0.tv_sec) * 1000
                + (wall_t1.tv_nsec - wall_t0.tv_nsec) / 1000000;
        if (ms >= 25) break;
    }

    printf("=== clock_getres / clock_gettime ===\n");
    for (size_t i = 0; i < sizeof(clocks)/sizeof(clocks[0]); i++) {
        test_getres_gettime(&clocks[i]);
    }

    printf("=== clock_nanosleep TIMER_ABSTIME past ===\n");
    for (size_t i = 0; i < sizeof(clocks)/sizeof(clocks[0]); i++) {
        test_nanosleep_abstime_past(&clocks[i]);
    }

    printf("=== clock_nanosleep relative / future ===\n");
    test_nanosleep_relative_short();
    test_nanosleep_abstime_future();

    printf("=== clock_getcpuclockid / pthread_getcpuclockid ===\n");
    test_getcpuclockid_self();
    test_getcpuclockid_getpid();
    test_pthread_getcpuclockid();

    printf("=== misc ===\n");
    test_invalid_clockid();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
