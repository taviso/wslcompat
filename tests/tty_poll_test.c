/* poll/select/pselect on a wrapped tty must see the underlying tty's
 * readiness rather than the synth O_PATH backing. Without the shim
 * the unshimmed run watches a real tty directly; with the shim
 * (after TCSETS clears ICANON) the fd is replaced with a synth fd
 * that the kernel can't wait on, so the wrappers in poll.c must
 * translate it back to the real tty fd transparently. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <err.h>
#include <sys/resource.h>
#include "testutil.h"

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

/* Take s out of canonical mode to trigger the synth wrap install when
 * running under the shim. On native Linux this is just a normal raw
 * TCSETS with no side effects. */
static void set_raw(int fd, cc_t vmin, cc_t vtime)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = vmin;
    t.c_cc[VTIME] = vtime;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

/* poll on an empty wrapped tty must report no readiness within the
 * timeout, then become readable after a write from the master. */
static void test_poll_basic(void)
{
    STATUS("--- poll: empty -> timeout, then readable after write ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    struct pollfd pfd = { .fd = s, .events = POLLIN };

    int r = poll(&pfd, 1, 100);
    CHECK(r == 0, "empty poll(timeout=100ms) -> %d (expected 0)", r);
    CHECK(pfd.fd == s,
          "  .fd preserved (%d, expected %d)", pfd.fd, s);

    if (write(m, "hi", 2) != 2) err(EXIT_FAILURE, "write");

    pfd.fd = s;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, 500);
    CHECK(r == 1, "poll after write -> %d (expected 1)", r);
    CHECK(pfd.revents & POLLIN, "  POLLIN set (revents=0x%x)", pfd.revents);
    CHECK(pfd.fd == s, "  .fd preserved (%d)", pfd.fd);

    close(m); close(s);
}

/* select must reflect readiness on the synth fd in the read set. */
static void test_select_basic(void)
{
    STATUS("--- select: empty -> timeout, then readable after write ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    fd_set rfds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);

    int r = select(s + 1, &rfds, NULL, NULL, &tv);
    CHECK(r == 0, "empty select(100ms) -> %d (expected 0)", r);
    CHECK(!FD_ISSET(s, &rfds), "  fd %d cleared on timeout", s);

    if (write(m, "hi", 2) != 2) err(EXIT_FAILURE, "write");

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = 0; tv.tv_usec = 500000;
    r = select(s + 1, &rfds, NULL, NULL, &tv);
    CHECK(r == 1, "select after write -> %d (expected 1)", r);
    CHECK(FD_ISSET(s, &rfds), "  fd %d set in rfds after write", s);

    close(m); close(s);
}

/* pselect mirrors select; quick sanity check that the shim wires up. */
static void test_pselect_basic(void)
{
    STATUS("--- pselect: readable after write ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    if (write(m, "x", 1) != 1) err(EXIT_FAILURE, "write");

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    struct timespec tmo = { .tv_sec = 0, .tv_nsec = 500000000 };

    int r = pselect(s + 1, &rfds, NULL, NULL, &tmo, NULL);
    CHECK(r == 1, "pselect after write -> %d (expected 1)", r);
    CHECK(FD_ISSET(s, &rfds), "  fd %d set in rfds", s);

    close(m); close(s);
}

/* Mixed array: a wrapped tty alongside a plain pipe. Both must report
 * independent readiness and the .fd values must come back unchanged. */
static void test_poll_mixed(void)
{
    STATUS("--- poll mixed: wrapped tty + plain pipe ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    int pipefd[2];
    if (pipe(pipefd) < 0) err(EXIT_FAILURE, "pipe");

    struct pollfd pfds[2] = {
        { .fd = s,         .events = POLLIN },
        { .fd = pipefd[0], .events = POLLIN },
    };

    int r = poll(pfds, 2, 100);
    CHECK(r == 0, "both empty -> %d (expected 0)", r);

    if (write(pipefd[1], "p", 1) != 1) err(EXIT_FAILURE, "pipe write");

    pfds[0].fd = s; pfds[0].events = POLLIN; pfds[0].revents = 0;
    pfds[1].fd = pipefd[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
    r = poll(pfds, 2, 500);
    CHECK(r == 1, "only pipe readable -> %d (expected 1)", r);
    CHECK(!(pfds[0].revents & POLLIN),
          "  tty not readable (revents=0x%x)", pfds[0].revents);
    CHECK(pfds[1].revents & POLLIN,
          "  pipe readable (revents=0x%x)", pfds[1].revents);
    CHECK(pfds[0].fd == s && pfds[1].fd == pipefd[0],
          "  .fd values preserved");

    /* Drain pipe; now write to tty; tty becomes readable. */
    char drain[8];
    (void)read(pipefd[0], drain, sizeof(drain));
    if (write(m, "t", 1) != 1) err(EXIT_FAILURE, "tty write");

    pfds[0].revents = pfds[1].revents = 0;
    r = poll(pfds, 2, 500);
    CHECK(r == 1, "only tty readable -> %d (expected 1)", r);
    CHECK(pfds[0].revents & POLLIN,
          "  tty readable (revents=0x%x)", pfds[0].revents);
    CHECK(!(pfds[1].revents & POLLIN),
          "  pipe not readable (revents=0x%x)", pfds[1].revents);

    close(pipefd[0]); close(pipefd[1]);
    close(m); close(s);
}

/* Both a wrapped tty AND a real pipe legitimately ready in the same
 * call: the kernel's first-call result flags both (bogus tty POLLIN
 * from the O_PATH canary + legit pipe POLLIN). The retry must
 * preserve the pipe's POLLIN -- if we accidentally swallowed real-fd
 * revents during the retry path, this is where we'd notice. */
static void test_poll_both_ready(void)
{
    STATUS("--- poll mixed: wrapped tty AND pipe both ready ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    int pipefd[2];
    if (pipe(pipefd) < 0) err(EXIT_FAILURE, "pipe");

    if (write(m, "t", 1) != 1) err(EXIT_FAILURE, "tty write");
    if (write(pipefd[1], "p", 1) != 1) err(EXIT_FAILURE, "pipe write");
    usleep(50000);

    struct pollfd pfds[2] = {
        { .fd = s,         .events = POLLIN },
        { .fd = pipefd[0], .events = POLLIN },
    };

    int r = poll(pfds, 2, 500);
    CHECK(r == 2, "both ready -> %d (expected 2)", r);
    CHECK(pfds[0].revents & POLLIN,
          "  tty readable (revents=0x%x)", pfds[0].revents);
    CHECK(pfds[1].revents & POLLIN,
          "  pipe readable (revents=0x%x)", pfds[1].revents);
    CHECK(pfds[0].fd == s && pfds[1].fd == pipefd[0],
          "  .fd values preserved");

    close(pipefd[0]); close(pipefd[1]);
    close(m); close(s);
}

/* Two independent wrapped ttys both ready in the same call: exercises
 * the swap table with multiple entries and checks that revents on
 * each synth fd corresponds to its own backing (we don't cross the
 * wires between swaps). */
static void test_poll_two_synth(void)
{
    STATUS("--- poll: two wrapped ttys both readable ---");
    int m1, s1, m2, s2;
    open_pty(&m1, &s1);
    open_pty(&m2, &s2);
    set_raw(s1, 0, 0);
    set_raw(s2, 0, 0);

    if (write(m1, "1", 1) != 1) err(EXIT_FAILURE, "tty1 write");
    if (write(m2, "2", 1) != 1) err(EXIT_FAILURE, "tty2 write");
    usleep(50000);

    struct pollfd pfds[2] = {
        { .fd = s1, .events = POLLIN },
        { .fd = s2, .events = POLLIN },
    };

    int r = poll(pfds, 2, 500);
    CHECK(r == 2, "both synth ttys ready -> %d (expected 2)", r);
    CHECK(pfds[0].revents & POLLIN,
          "  tty1 readable (revents=0x%x)", pfds[0].revents);
    CHECK(pfds[1].revents & POLLIN,
          "  tty2 readable (revents=0x%x)", pfds[1].revents);
    CHECK(pfds[0].fd == s1 && pfds[1].fd == s2,
          "  caller-visible .fd values preserved");

    /* Now drain only tty1, re-poll: tty1 not ready, tty2 still ready.
     * Catches a bug where we'd reflect tty2's readiness onto tty1. */
    char buf[4];
    (void)read(s1, buf, sizeof(buf));

    pfds[0].revents = pfds[1].revents = 0;
    r = poll(pfds, 2, 500);
    CHECK(r == 1, "after draining tty1 -> %d (expected 1)", r);
    CHECK(!(pfds[0].revents & POLLIN),
          "  tty1 not readable (revents=0x%x)", pfds[0].revents);
    CHECK(pfds[1].revents & POLLIN,
          "  tty2 still readable (revents=0x%x)", pfds[1].revents);

    close(m1); close(s1); close(m2); close(s2);
}

/* Negative fd entries (poll(2) ignores them) must survive the swap
 * pass untouched. */
static void test_poll_negative_fd(void)
{
    STATUS("--- poll: negative fd ignored ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    if (write(m, "z", 1) != 1) err(EXIT_FAILURE, "write");

    struct pollfd pfds[2] = {
        { .fd = -1, .events = POLLIN },
        { .fd = s,  .events = POLLIN },
    };

    int r = poll(pfds, 2, 500);
    CHECK(r == 1, "poll -> %d (expected 1)", r);
    CHECK(pfds[0].fd == -1, "  negative .fd untouched");
    CHECK(pfds[1].revents & POLLIN, "  tty readable");

    close(m); close(s);
}

static void test_poll_events_zero(void)
{
    STATUS("--- poll: events=0 on synth fd (must not block indefinitely) ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    // Close master so slave gets POLLHUP
    close(m);

    struct pollfd pfd = { .fd = s, .events = 0, .revents = 0 };
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    // timeout is 500ms
    int r = poll(&pfd, 1, 500);
    
    gettimeofday(&end, NULL);
    long diff = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;
    
    CHECK(r == 1, "poll rv == 1 (got %d)", r);
    CHECK(pfd.revents & POLLHUP, "poll revents contains POLLHUP (got 0x%x)", pfd.revents);
    CHECK(diff < 250, "poll returned immediately (took %ld ms)", diff);

    close(s);
}

static void test_select_high_fd(void)
{
    STATUS("--- select: ready fd >= FD_SETSIZE alongside synth fd ---");
    
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < FD_SETSIZE + 32) {
        rl.rlim_cur = FD_SETSIZE + 32;
        rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            STATUS("  SKIP: cannot raise RLIMIT_NOFILE");
            return;
        }
    }

    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) err(EXIT_FAILURE, "open /dev/null");

    int high = -1;
    for (int i = 0; i < FD_SETSIZE + 10; i++) {
        high = dup(devnull);
        if (high >= FD_SETSIZE + 5) break;
    }
    if (high < FD_SETSIZE) {
        STATUS("  SKIP: cannot get high fd");
        close(devnull);
        close(m); close(s);
        return;
    }

    size_t set_bytes = (high + 64) / 8;
    fd_set *r = calloc(1, set_bytes);
    
    unsigned long *bits = (unsigned long *)r;
    // Set high fd (always ready because it's /dev/null)
    bits[high / (8 * sizeof(unsigned long))] |= (1UL << (high % (8 * sizeof(unsigned long))));
    // Set synth fd (not ready)
    bits[s / (8 * sizeof(unsigned long))] |= (1UL << (s % (8 * sizeof(unsigned long))));
    
    int maxfd = high > s ? high : s;

    STATUS("  s=%d, high=%d, maxfd=%d", s, high, maxfd);

    struct timeval tv = { 0, 100000 }; // 100ms
    int rv = select(maxfd + 1, r, NULL, NULL, &tv);
    
    // high is ready, s is not ready.
    CHECK(rv == 1, "select rv == 1 (got %d, errno=%d)", rv, errno);
    CHECK(bits[high / (8 * sizeof(unsigned long))] & (1UL << (high % (8 * sizeof(unsigned long)))), 
          "  high fd is ready");
    
    free(r);
    close(high);
    close(devnull);
    close(m); close(s);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: tty_poll_test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(5);

    test_poll_basic();
    test_select_basic();
    test_pselect_basic();
    test_poll_mixed();
    test_poll_both_ready();
    test_poll_two_synth();
    test_poll_negative_fd();
    test_poll_events_zero();
    test_select_high_fd();

    TEST_REPORT();
}
