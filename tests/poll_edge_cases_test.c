#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <stdint.h>
#include <unistd.h>
#include <err.h>

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

static void set_raw(int fd)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

static volatile sig_atomic_t got_usr1;
static void handle_usr1(int sig) { (void)sig; got_usr1 = 1; }

extern int __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout,
                      size_t fdslen);
extern int __ppoll_chk(struct pollfd *fds, nfds_t nfds,
                       const struct timespec *tmo, const sigset_t *sigmask,
                       size_t fdslen);

/* ppoll with a signal mask must correctly block/unblock signals while
 * waiting on a synthetic fd. */
static void test_ppoll_sigmask(void)
{
    STATUS("--- ppoll: sigmask handling with synthetic fd ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    got_usr1 = 0;
    struct sigaction sa = { .sa_handler = handle_usr1 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t mask, orig;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, &orig);

    struct pollfd pfd = { .fd = s, .events = POLLIN };
    struct timespec tmo = { .tv_sec = 0, .tv_nsec = 100000000 };

    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == 0, "SIGUSR1 blocked before ppoll");

    sigset_t empty;
    sigemptyset(&empty);
    int r = ppoll(&pfd, 1, &tmo, &empty);
    CHECK(r == -1 && errno == EINTR, "ppoll returned EINTR (got %d, errno=%d)", r, errno);
    CHECK(got_usr1 == 1, "SIGUSR1 delivered during ppoll");

    sigprocmask(SIG_SETMASK, &orig, NULL);
    close(m); close(s);
}

/* pselect similarly must handle the signal mask correctly. */
static void test_pselect_sigmask(void)
{
    STATUS("--- pselect: sigmask handling with synthetic fd ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    got_usr1 = 0;
    struct sigaction sa = { .sa_handler = handle_usr1 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t mask, orig;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, &orig);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    struct timespec tmo = { .tv_sec = 0, .tv_nsec = 100000000 };

    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == 0, "SIGUSR1 blocked before pselect");

    sigset_t empty;
    sigemptyset(&empty);
    int r = pselect(s + 1, &rfds, NULL, NULL, &tmo, &empty);
    CHECK(r == -1 && errno == EINTR, "pselect returned EINTR (got %d, errno=%d)", r, errno);
    CHECK(got_usr1 == 1, "SIGUSR1 delivered during pselect");

    sigprocmask(SIG_SETMASK, &orig, NULL);
    close(m); close(s);
}

/* select/pselect MUST NOT try to translate fds >= FD_SETSIZE. */
static void test_select_fd_limit(void)
{
    STATUS("--- select: fds >= FD_SETSIZE ---");

    fd_set rfds;
    FD_ZERO(&rfds);

    struct timeval tv = { 0, 0 };
    int r = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
    CHECK(r == 0, "select(FD_SETSIZE) -> %d", r);

    struct timespec tmo = { 0, 0 };
    r = pselect(FD_SETSIZE, &rfds, NULL, NULL, &tmo, NULL);
    CHECK(r == 0, "pselect(FD_SETSIZE) -> %d", r);
}

/* poll on a synth fd that doesn't provide a pollable backing must be
 * allowed to pass through to the kernel (which will report POLLNVAL
 * or similar for the O_PATH backing). */
static void test_poll_non_pollable(void)
{
    STATUS("--- poll: synth fd without pollable backing ---");

    int fd = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            printf("  SKIP: /proc/sys/vm/mmap_min_addr not found\n");
            return;
        }
        err(EXIT_FAILURE, "open mmap_min_addr");
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, 0);

    CHECK(r == 1, "poll(non-pollable synth) -> %d", r);
    CHECK(pfd.revents != 0, "revents is non-zero (0x%x)", pfd.revents);

    close(fd);
}

/* POLLPRI is outside DEFAULT_POLLMASK (POLLIN|POLLOUT|POLLRDNORM|
 * POLLWRNORM). On a synth O_PATH fd the kernel would never report
 * POLLPRI, so the shim's proactive swap path must detect the
 * non-overlapping events and translate before the kernel call.
 * Without the proactive swap the call would block until timeout. */
static void test_poll_pri_proactive(void)
{
    STATUS("--- poll: POLLPRI triggers proactive swap ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    struct pollfd pfd = { .fd = s, .events = POLLPRI };
    int r = poll(&pfd, 1, 100);
    CHECK(r == 0, "poll(POLLPRI, empty tty, 100ms) -> %d (expected 0)", r);
    CHECK(pfd.fd == s, ".fd preserved (%d)", pfd.fd);

    close(m); close(s);
}

/* POLLOUT is within DEFAULT_POLLMASK, so the canary path handles it:
 * the kernel reports readiness on the O_PATH backing (which always
 * appears writable). canary_fired detects the synth fd, retries with
 * the real backing, and returns the real tty's readiness. */
static void test_poll_out_canary(void)
{
    STATUS("--- poll: POLLOUT triggers canary retry ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    struct pollfd pfd = { .fd = s, .events = POLLOUT };
    int r = poll(&pfd, 1, 200);
    CHECK(r == 1, "poll(POLLOUT, writable tty) -> %d (expected 1)", r);
    CHECK(pfd.revents & POLLOUT, "POLLOUT set (revents=0x%x)", pfd.revents);
    CHECK(pfd.fd == s, ".fd preserved (%d)", pfd.fd);

    close(m); close(s);
}

/* select writefds: the canary (EPERM on WSL1) fires, synth fds are
 * swapped for their real backings, and the retry correctly reports
 * the tty as writable in the caller's fd_set. */
static void test_poll_bad_pointer(void)
{
    STATUS("--- poll: bad array returns EFAULT with synth active ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    int (*poll_fn)(struct pollfd *, nfds_t, int) = poll;
    errno = 0;
    int r = poll_fn((struct pollfd *)(uintptr_t)1, 1, 0);
    CHECK(r == -1 && errno == EFAULT,
          "poll(bad pointer) -> %d errno=%d (expected -1/EFAULT)",
          r, errno);

    close(m); close(s);
}

static void test_select_writefds(void)
{
    STATUS("--- select: writefds on synth fd ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    struct timeval tv = { 0, 200000 };
    int r = select(s + 1, NULL, &wfds, NULL, &tv);
    CHECK(r == 1, "select(writefds, writable tty) -> %d (expected 1)", r);
    CHECK(FD_ISSET(s, &wfds), "fd %d set in writefds", s);

    close(m); close(s);
}

/* select exceptfds (POLLPRI equivalent): no exceptional condition on
 * an idle tty, so the retry after EPERM canary should time out. */
static void test_select_exceptfds(void)
{
    STATUS("--- select: exceptfds on synth fd ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    fd_set efds;
    FD_ZERO(&efds);
    FD_SET(s, &efds);
    struct timeval tv = { 0, 100000 };
    int r = select(s + 1, NULL, NULL, &efds, &tv);
    CHECK(r == 0, "select(exceptfds, empty tty) -> %d (expected 0)", r);
    CHECK(!FD_ISSET(s, &efds), "fd %d not set in exceptfds", s);

    close(m); close(s);
}

/* More synth fds than MAX_SWAPS (64) in a single select call: the
 * swap-in pass leaves O_PATH fds beyond the cap untranslated, so on
 * WSL1 the retry still hits EPERM. That is a known limitation --
 * the test accepts EPERM as non-fatal. The important property is
 * that the shim does not crash or corrupt memory. */
#define SEL_OVERFLOW_FDS 68

static void test_select_max_swaps(void)
{
    STATUS("--- select: >64 synth fds (MAX_SWAPS boundary) ---");

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < SEL_OVERFLOW_FDS * 2 + 16) {
        rl.rlim_cur = SEL_OVERFLOW_FDS * 2 + 16;
        if (rl.rlim_cur > rl.rlim_max)
            rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            printf("  SKIP: cannot raise RLIMIT_NOFILE\n");
            return;
        }
    }

    int *masters = calloc(SEL_OVERFLOW_FDS, sizeof(int));
    int *slaves  = calloc(SEL_OVERFLOW_FDS, sizeof(int));
    if (!masters || !slaves)
        err(EXIT_FAILURE, "calloc");
    int maxfd = -1;
    int opened = 0;

    for (int i = 0; i < SEL_OVERFLOW_FDS; i++) {
        int m, s;
        m = posix_openpt(O_RDWR | O_NOCTTY);
        if (m < 0) break;
        if (grantpt(m) < 0 || unlockpt(m) < 0) {
            close(m);
            break;
        }
        char *name = ptsname(m);
        if (!name) { close(m); break; }
        s = open(name, O_RDWR | O_NOCTTY);
        if (s < 0) { close(m); break; }
        struct termios t;
        if (tcgetattr(s, &t) < 0) { close(m); close(s); break; }
        t.c_lflag &= ~ICANON;
        if (tcsetattr(s, TCSANOW, &t) < 0) { close(m); close(s); break; }
        masters[i] = m;
        slaves[i]  = s;
        opened++;
        if (s > maxfd) maxfd = s;
    }

    if (opened < 2) {
        printf("  SKIP: could not open enough ptys (got %d)\n", opened);
        for (int i = 0; i < opened; i++) {
            close(masters[i]);
            close(slaves[i]);
        }
        free(masters); free(slaves);
        return;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    for (int i = 0; i < opened; i++)
        FD_SET(slaves[i], &rfds);

    struct timeval tv = { 0, 200000 };
    int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    CHECK(r == 0 || (r == -1 && errno == EPERM),
          "select(%d synth ttys) -> %d (errno=%d)%s",
          opened, r, r < 0 ? errno : 0,
          (r == -1 && errno == EPERM) ? " [MAX_SWAPS EPERM, known]" : "");

    for (int i = 0; i < opened; i++) {
        close(masters[i]);
        close(slaves[i]);
    }
    free(masters); free(slaves);
}

/* FORTIFY_SOURCE redirects poll/ppoll through __poll_chk/__ppoll_chk.
 * Explicit calls with correct bounds must forward to the real poll
 * shim and produce the same result as calling poll() directly. */
static void test_poll_chk_forward(void)
{
    STATUS("--- __poll_chk / __ppoll_chk forward correctly ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s);

    struct pollfd pfd = { .fd = s, .events = POLLIN };
    struct timespec tmo = { 0, 50 * 1000 * 1000 };

    int r = __poll_chk(&pfd, 1, 50, sizeof(pfd));
    CHECK(r == 0, "__poll_chk(empty tty, 50ms) -> %d (expected 0)", r);
    CHECK(pfd.fd == s, ".fd preserved (%d)", pfd.fd);

    r = __ppoll_chk(&pfd, 1, &tmo, NULL, sizeof(pfd));
    CHECK(r == 0, "__ppoll_chk(empty tty, 50ms) -> %d (expected 0)", r);

    close(m); close(s);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: poll_edge_cases timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(15);

    test_ppoll_sigmask();
    test_pselect_sigmask();
    test_select_fd_limit();
    test_poll_non_pollable();
    test_poll_pri_proactive();
    test_poll_out_canary();
    test_poll_bad_pointer();
    test_select_writefds();
    test_select_exceptfds();
    test_select_max_swaps();
    test_poll_chk_forward();

    TEST_REPORT();
}
