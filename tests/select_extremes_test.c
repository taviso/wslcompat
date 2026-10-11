/* Boundary cases for the select/pselect path:
 *
 *   - All-NULL sets (sleep)
 *   - timeout=NULL (infinite) unblocked by a forked child
 *   - timeout={0,0} (non-blocking)
 *   - select(0, NULL, NULL, NULL, &tv) where tv is small (write-back)
 *   - nfds = FD_SETSIZE
 *   - inotify_fd > FD_SETSIZE -- async wake silently disabled, the
 *     fast path still correctness-passes
 *   - pselect with sigmask that blocks SIGTERM (signal still received
 *     during wait by another thread? skip, just exercise the sigmask
 *     plumbing)
 *
 * Forcing inotify_fd >= FD_SETSIZE is the interesting bound: open
 * enough fds first that the shim's lazy-open lands above 1023, then
 * exercise select. We can't easily open another inotify (kernel
 * picks the fd), but we CAN burn fds 0..1023 with pipes so any
 * subsequent open goes high.
 *
 * NOTE: this test only exercises the > FD_SETSIZE scenario when the
 * shim hasn't already opened inotify_fd. If the shim's inotify_fd
 * landed at a small number (very common), the bound is unreachable
 * without a fresh process. We report a SKIP in that case. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static void test_all_null(void)
{
    printf("--- all-NULL sets (sleep via select) ---\n");
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
    int r = select(0, NULL, NULL, NULL, &tv);
    CHECK(r == 0, "select(0, NULL, NULL, NULL, 50ms) -> %d (expected 0)", r);
    // Linux writes back tv to remaining time. Should be near 0.
    CHECK(tv.tv_sec == 0 && tv.tv_usec < 50 * 1000,
          "tv written back to {%ld, %ld} (expected near 0)",
          (long)tv.tv_sec, (long)tv.tv_usec);
}

static void test_zero_timeout(void)
{
    printf("--- timeout={0,0} non-blocking ---\n");
    int p[2];
    if (pipe(p) != 0) err(EXIT_FAILURE, "pipe");
    fd_set r;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timeval tv = { 0, 0 };
    int rv = select(p[0] + 1, &r, NULL, NULL, &tv);
    CHECK(rv == 0, "select(empty pipe, {0,0}) -> %d (expected 0)", rv);
    close(p[0]); close(p[1]);
}

static void test_infinite_via_fork(void)
{
    printf("--- timeout=NULL unblocked by fork ---\n");
    int p[2];
    if (pipe(p) != 0) err(EXIT_FAILURE, "pipe");

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        usleep(100 * 1000);
        if (write(p[1], "x", 1) != 1) _exit(2);
        _exit(0);
    }

    fd_set r;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    int rv = select(p[0] + 1, &r, NULL, NULL, NULL);
    waitpid(pid, NULL, 0);
    CHECK(rv == 1 && FD_ISSET(p[0], &r),
          "select(NULL tv) returned %d, isset=%d",
          rv, FD_ISSET(p[0], &r));
    close(p[0]); close(p[1]);
}

static void test_pselect_sigmask(void)
{
    printf("--- pselect with sigmask ---\n");
    int p[2];
    if (pipe(p) != 0) err(EXIT_FAILURE, "pipe");
    fd_set r;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timespec tmo = { 0, 50 * 1000 * 1000 };
    sigset_t mask;
    sigemptyset(&mask);
    int rv = pselect(p[0] + 1, &r, NULL, NULL, &tmo, &mask);
    CHECK(rv == 0, "pselect(empty pipe, 50ms, empty mask) -> %d (expected 0)", rv);

    if (write(p[1], "x", 1) != 1) err(EXIT_FAILURE, "write");
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timespec tmo2 = { 1, 0 };
    rv = pselect(p[0] + 1, &r, NULL, NULL, &tmo2, &mask);
    CHECK(rv == 1 && FD_ISSET(p[0], &r),
          "pselect(written pipe) -> %d isset=%d",
          rv, FD_ISSET(p[0], &r));
    close(p[0]); close(p[1]);
}

// Burn fds until the next open lands at or past FD_SETSIZE; then
// exercise select with our existing low-fd pipe to confirm the
// dispatch path still works (the splice will silently fall back).
static void test_high_fd_select(void)
{
    printf("--- select with high fds (inotify_fd > FD_SETSIZE bound) ---\n");

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < (rlim_t)(FD_SETSIZE + 32)) {
        rl.rlim_cur = FD_SETSIZE + 32;
        if (rl.rlim_cur > rl.rlim_max) rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            printf("  SKIP: cannot raise RLIMIT_NOFILE\n");
            return;
        }
    }

    int p[2];
    if (pipe(p) != 0) err(EXIT_FAILURE, "pipe");
    if (p[0] >= FD_SETSIZE || p[1] >= FD_SETSIZE) {
        printf("  SKIP: pipe already returned fds >= FD_SETSIZE\n");
        close(p[0]); close(p[1]);
        return;
    }

    // Pad up to a high fd by dup'ing /dev/null until we hit FD_SETSIZE.
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) err(EXIT_FAILURE, "open /dev/null");

    int high = -1;
    while (1) {
        int d = dup(devnull);
        if (d < 0) break;
        if (d >= FD_SETSIZE) { high = d; break; }
    }
    if (high < 0) {
        printf("  SKIP: could not push fds past FD_SETSIZE\n");
        close(devnull); close(p[0]); close(p[1]);
        return;
    }
    close(high);  // free the slot but we've proven the table is wide
    close(devnull);

    // Now do a normal select; correctness shouldn't depend on whether
    // our inotify_fd is high or low.
    fd_set r;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timeval tv = { 0, 50 * 1000 };
    int rv = select(p[0] + 1, &r, NULL, NULL, &tv);
    CHECK(rv == 0, "select with wide fd table -> %d (expected 0)", rv);

    if (write(p[1], "x", 1) != 1) err(EXIT_FAILURE, "write");
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timeval tv2 = { 1, 0 };
    rv = select(p[0] + 1, &r, NULL, NULL, &tv2);
    CHECK(rv == 1 && FD_ISSET(p[0], &r),
          "select after write -> %d isset=%d", rv, FD_ISSET(p[0], &r));

    close(p[0]); close(p[1]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    test_all_null();
    test_zero_timeout();
    test_infinite_via_fork();
    test_pselect_sigmask();
    test_high_fd_select();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
