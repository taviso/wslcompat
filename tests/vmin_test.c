#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <err.h>
#include "testutil.h"

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: test timed out (kernel ignoring VMIN/VTIME?)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

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

static void set_raw(int fd, cc_t vmin, cc_t vtime)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = vmin;
    t.c_cc[VTIME] = vtime;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

static long elapsed_ms(struct timeval *t0, struct timeval *t1)
{
    return (t1->tv_sec  - t0->tv_sec)  * 1000
         + (t1->tv_usec - t0->tv_usec) / 1000;
}

static void test_polled(void)
{
    STATUS("--- VMIN=0/VTIME=0 (polled, instant return) ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    char buf[16];
    struct timeval t0, t1;

    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    STATUS("  empty read -> %zd bytes in %ld ms", n, ms);
    CHECK(n == 0 && ms <= 200, "expected 0 bytes instantly");

    if (write(m, "abc", 3) != 3) err(EXIT_FAILURE, "write");
    usleep(50000);

    gettimeofday(&t0, NULL);
    n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    ms = elapsed_ms(&t0, &t1);
    STATUS("  3-byte read -> %zd bytes in %ld ms", n, ms);
    close(m); close(s);
    CHECK(n == 3 && ms <= 200, "expected 3 bytes instantly");
}

static void test_vmin_only(void)
{
    STATUS("--- VMIN=5/VTIME=0 (block until N bytes) ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 5, 0);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        usleep(300000);
        write(m, "hello", 5);
        _exit(0);
    }

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);

    waitpid(pid, NULL, 0);
    close(m); close(s);

    STATUS("  read -> %zd bytes in %ld ms", n, ms);
    CHECK(n >= 5 && ms >= 200, "expected at least 5 bytes and kernel honored VMIN");
}

static void test_interbyte_timeout(void)
{
    STATUS("--- VMIN=10/VTIME=5 (inter-byte timeout after first byte) ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 10, 5);

    if (write(m, "xyz", 3) != 3) err(EXIT_FAILURE, "write");

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    close(m); close(s);

    STATUS("  read -> %zd bytes in %ld ms", n, ms);
    CHECK(n == 3 && ms >= 350 && ms <= 1500, "expected exactly 3 bytes and timing within range [350, 1500]");
}

static void test_nonblock_precedence(void)
{
    STATUS("--- O_NONBLOCK takes precedence over VMIN ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 5, 0);

    int flags = fcntl(s, F_GETFL);
    if (flags == -1) err(EXIT_FAILURE, "F_GETFL");
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1)
        err(EXIT_FAILURE, "F_SETFL");

    char buf[16];
    struct timeval t0, t1;
    errno = 0;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    int saved_errno = errno;

    close(m);
    close(s);

    STATUS("  empty nonblocking read -> %zd errno=%d in %ld ms", n, saved_errno, ms);

    CHECK(n == -1 && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) && ms <= 200, "expected EAGAIN/EWOULDBLOCK and fast return");
}

static void test_nonblock_at_install(void)
{
    /* O_NONBLOCK already set on the fd when TCSETS fires exercises
     * the install path that primes the nonblock cache via F_GETFL.
     * If priming regressed (cache stayed at 0), the slow VMIN polling
     * path would block waiting for data and the watchdog would fire. */
    STATUS("--- O_NONBLOCK at open, then TCSETS to noncanon ---");

    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s == -1) err(EXIT_FAILURE, "open slave");

    /* TCSETS triggers tty_install; the cache must be primed to
     * nonblock=1 from F_GETFL or the read below will hang. */
    set_raw(s, 5, 0);

    char buf[16];
    struct timeval t0, t1;
    errno = 0;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    int saved = errno;

    close(m); close(s);

    STATUS("  empty nonblocking read -> %zd errno=%d ms=%ld", n, saved, ms);

    CHECK(n == -1 && (saved == EAGAIN || saved == EWOULDBLOCK) && ms <= 200, "expected -1/EAGAIN and cache primed");
}

static void test_vtime_precision(void)
{
    /* VMIN=0, VTIME=N: read should block for ~N*100ms then return 0.
     * Verify the kernel honors VTIME at a few values. Tolerance ±150ms
     * around the expected hold to absorb scheduler jitter under load. */
    static const cc_t vtimes[] = { 1, 5, 10 };

    for (size_t i = 0; i < sizeof(vtimes) / sizeof(vtimes[0]); i++) {
        cc_t v = vtimes[i];
        long want_ms = (long)v * 100;
        STATUS("--- VTIME=%u (~%ldms) ---", v, want_ms);

        int m, s;
        open_pty(&m, &s);
        set_raw(s, 0, v);

        char buf[16];
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        ssize_t n = read(s, buf, sizeof(buf));
        gettimeofday(&t1, NULL);
        long ms = elapsed_ms(&t0, &t1);

        close(m); close(s);

        STATUS("  read -> %zd in %ldms (want ~%ldms ±150)", n, ms, want_ms);
        CHECK(n == 0 && ms >= want_ms - 150 && ms <= want_ms + 150, "expected 0 timeout and timing in window");
    }
}

static void test_large_count_small_vmin(void)
{
    /* count >> VMIN: the read should return as soon as VMIN bytes
     * are available, NOT wait to fill the (much larger) buffer. */
    STATUS("--- VMIN=2, write 5, read with count=1024 ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 2, 0);

    if (write(m, "hello", 5) != 5) err(EXIT_FAILURE, "write");
    usleep(50000);

    char buf[1024];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);

    close(m); close(s);

    STATUS("  read -> %zd in %ldms", n, ms);
    CHECK(n >= 2 && n <= 5 && ms <= 200, "expected 2..5 bytes and no wait for full buffer");
}

static void test_nonblock_matrix(void)
{
    /* O_NONBLOCK overrides MIN/TIME for all four combinations: the
     * read MUST NOT block on empty input. The return value differs
     * across modes (Linux returns 0 for VMIN=0 modes because VMIN=0
     * is itself "return whatever's available, even 0 bytes", while
     * for VMIN>0 modes O_NONBLOCK yields -1/EAGAIN). What we're
     * asserting is "didn't block" -- either result is fine. */
    struct {
        cc_t vmin, vtime;
        const char *name;
    } cases[] = {
        { 0, 0, "VMIN=0/VTIME=0" },
        { 0, 5, "VMIN=0/VTIME=5" },
        { 5, 0, "VMIN=5/VTIME=0" },
        { 5, 5, "VMIN=5/VTIME=5" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        STATUS("--- O_NONBLOCK + %s (must not block) ---", cases[i].name);
        int m, s;
        open_pty(&m, &s);
        set_raw(s, cases[i].vmin, cases[i].vtime);

        int flags = fcntl(s, F_GETFL);
        if (flags == -1) err(EXIT_FAILURE, "F_GETFL");
        if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1)
            err(EXIT_FAILURE, "F_SETFL");

        char buf[16];
        struct timeval t0, t1;
        errno = 0;
        gettimeofday(&t0, NULL);
        ssize_t n = read(s, buf, sizeof(buf));
        gettimeofday(&t1, NULL);
        long ms = elapsed_ms(&t0, &t1);
        int saved = errno;

        close(m); close(s);

        STATUS("  read -> %zd errno=%d ms=%ld", n, saved, ms);
        int ok_zero = (n == 0);
        int ok_eagain = (n == -1 && (saved == EAGAIN || saved == EWOULDBLOCK));
        CHECK((ok_zero || ok_eagain) && ms <= 200, "expected 0 or -1/EAGAIN and fast return");
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    /* Suite budget: existing tests sum to ~1.5s, the new VTIME and
     * matrix subtests add ~2.5s, doubled to absorb valgrind overhead. */
    alarm(2);

    test_polled();
    alarm(2);
    test_vmin_only();
    alarm(2);
    test_interbyte_timeout();
    alarm(2);
    test_nonblock_precedence();
    alarm(2);
    test_nonblock_at_install();
    alarm(2);
    test_vtime_precision();
    alarm(2);
    test_large_count_small_vmin();
    alarm(2);
    test_nonblock_matrix();

    TEST_REPORT();
}
