/* Wrap-lifecycle tests for the tty shim. tty_install replaces a tty
 * fd with a synthetic O_PATH fd that forwards I/O to a duped real
 * fd; this file covers behaviors specific to that swap rather than
 * VMIN/VTIME semantics (those live in vmin_test.c). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
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

static void set_raw(int fd, cc_t vmin, cc_t vtime)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = vmin;
    t.c_cc[VTIME] = vtime;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

static void set_canon(int fd)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag |= ICANON;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

static long elapsed_ms(struct timeval *t0, struct timeval *t1)
{
    return (t1->tv_sec  - t0->tv_sec)  * 1000
         + (t1->tv_usec - t0->tv_usec) / 1000;
}

/* The shim installs the wrap on TCSETS+!ICANON and leaves it in place
 * across subsequent mode switches. The handler observes TCSETS to
 * maintain its canon hint; canonical-mode reads passthru to the
 * kernel, raw-mode reads go through the VMIN/VTIME logic. Round-trip
 * raw -> canon -> raw, checking ioctl observability throughout. */
static void test_canon_toggle(void)
{
    alarm(2);
    STATUS("--- ICANON toggle survives the wrap ---");
    int m, s;
    open_pty(&m, &s);

    set_raw(s, 0, 0);    /* installs wrap, canon=0 */

    struct termios t;
    CHECK(ioctl(s, TCGETS, &t) == 0,
          "TCGETS after wrap install -> ok");
    CHECK(!(t.c_lflag & ICANON),
          "  ICANON reported as cleared (raw)");

    /* Switch back to canonical -- handler should update its hint.
     * The wrap STAYS in place; we just route differently. */
    set_canon(s);
    CHECK(ioctl(s, TCGETS, &t) == 0 && (t.c_lflag & ICANON),
          "after set_canon: TCGETS reports ICANON set");

    /* And back to raw -- the wrap should still respond. */
    set_raw(s, 3, 0);
    CHECK(ioctl(s, TCGETS, &t) == 0 && !(t.c_lflag & ICANON),
          "after second set_raw: TCGETS reports ICANON cleared");
    CHECK(t.c_cc[VMIN] == 3,
          "  VMIN updated to 3 (got %u)", t.c_cc[VMIN]);

    /* Functional check: with VMIN=3 and 3 bytes available, read
     * should return promptly (cooked path). */
    if (write(m, "abc", 3) != 3) err(EXIT_FAILURE, "write");
    usleep(50000);

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);

    CHECK(n == 3 && ms < 200,
          "raw read after toggle cycle -> %zd bytes in %ldms", n, ms);

    close(m); close(s);
}

/* OFD-based dispatch must work via any fd that references the same
 * underlying entry. dup the wrapped fd, close the original, then
 * exercise the dup: read/ioctl should still route through the
 * handler because synth_pin_fd identifies by (dev, ino), not fd
 * number. */
static void test_dup_wrapped(void)
{
    alarm(2);
    STATUS("--- dup'd wrapped fd routes through the same handler ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    int dup_fd = dup(s);
    CHECK(dup_fd >= 0, "dup(wrapped fd) -> %d", dup_fd);
    if (dup_fd < 0) { close(m); close(s); return; }

    close(s);

    if (write(m, "xy", 2) != 2) err(EXIT_FAILURE, "write");
    usleep(50000);

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(dup_fd, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);

    CHECK(n == 2 && ms < 200,
          "read via dup after original closed -> %zd bytes in %ldms",
          n, ms);

    /* ioctl through the dup should also dispatch -- the kernel
     * would reject TCGETS on the O_PATH backing with EBADF, but
     * the synth dispatch forwards to real_fd. */
    struct termios t;
    CHECK(ioctl(dup_fd, TCGETS, &t) == 0,
          "ioctl(TCGETS) via dup -> dispatched to real tty");

    close(dup_fd); close(m);
}

/* Several wrapped ttys exist concurrently. The slot table and
 * per-entry userptr must keep their state isolated -- a TCSETS on
 * one wrapped fd must NOT affect another's canon hint, and reads
 * route to the right backing tty. */
#define MULTI_N 3
static void test_multiple_wraps(void)
{
    alarm(2);
    STATUS("--- %d concurrent wrapped ttys, independent state ---", MULTI_N);
    int m[MULTI_N], s[MULTI_N];

    for (int i = 0; i < MULTI_N; i++) {
        open_pty(&m[i], &s[i]);
        /* Distinct VMIN values so we can tell their states apart. */
        set_raw(s[i], (cc_t)(i + 1), 0);
    }

    for (int i = 0; i < MULTI_N; i++) {
        struct termios t;
        CHECK(ioctl(s[i], TCGETS, &t) == 0,
              "tty[%d] TCGETS via wrap -> ok", i);
        CHECK(t.c_cc[VMIN] == (cc_t)(i + 1),
              "  tty[%d] VMIN=%u (want %d)",
              i, t.c_cc[VMIN], i + 1);
    }

    /* Functional check: write VMIN bytes to each master, read from
     * each slave, verify isolation. */
    for (int i = 0; i < MULTI_N; i++) {
        char out[4] = "abcd";
        ssize_t want = i + 1;
        if (write(m[i], out, want) != want) err(EXIT_FAILURE, "write %d", i);
    }
    usleep(50000);

    for (int i = 0; i < MULTI_N; i++) {
        char buf[16];
        ssize_t n = read(s[i], buf, sizeof(buf));
        CHECK(n == i + 1,
              "  tty[%d] read returned %zd (want %d)", i, n, i + 1);
    }

    for (int i = 0; i < MULTI_N; i++) {
        close(m[i]); close(s[i]);
    }
}

/* Fork inherits the synth_table (it's in the .data segment, copied
 * by fork) and the wrapped fd (kernel inherits the fd table). The
 * child should therefore see the wrap and can dispatch through it.
 * This contradicts an older note about synth state being strictly
 * process-local -- that's true for exec (constructor re-runs into a
 * fresh table) and SCM_RIGHTS (receiver never had the entry), but
 * fork carries the state along. */
static void test_fork_inherits_wrap(void)
{
    alarm(2);
    STATUS("--- fork inherits wrap (parent and child both dispatch) ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    int sync_pipe[2];
    if (pipe(sync_pipe) < 0) err(EXIT_FAILURE, "pipe");

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        close(sync_pipe[0]);

        /* Wait for parent to write to master, then read via wrap. */
        usleep(100000);
        char buf[16];
        ssize_t n = read(s, buf, sizeof(buf));
        unsigned char ok = (n == 5 && memcmp(buf, "child", 5) == 0);
        write(sync_pipe[1], &ok, 1);
        _exit(ok ? 0 : 1);
    }

    close(sync_pipe[1]);

    /* Parent feeds the child. */
    if (write(m, "child", 5) != 5) err(EXIT_FAILURE, "write");

    unsigned char ok = 0;
    read(sync_pipe[0], &ok, 1);
    CHECK(ok, "child read via inherited wrap returned 5 bytes \"child\"");

    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child exited cleanly (status=0x%x)", status);

    close(sync_pipe[0]);
    close(m); close(s);
}

/* Visibility quirks: the wrap leaves divergent answers behind for
 * fd-introspection syscalls. After the dup2 swap, fstat reports the
 * synth O_PATH backing's tmpfile inode (S_ISREG), while a native
 * unshimmed tty reports S_ISCHR. ioctl(TCGETS) is forwarded by the
 * synth dispatch to the real tty either way, so isatty() (which
 * keys off tcgetattr) still returns true. The CHECK accepts both
 * S_ISREG (shimmed) and S_ISCHR (unshimmed/native) so it documents
 * the divergence without breaking the linux: target -- apps that
 * branch on fstat-type vs isatty are the ones to watch. */
static void test_visibility_quirks(void)
{
    alarm(2);
    STATUS("--- visibility: fstat vs isatty after wrap ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    struct stat st;
    CHECK(fstat(s, &st) == 0, "fstat on wrapped fd -> ok");
    CHECK(S_ISREG(st.st_mode) || S_ISCHR(st.st_mode),
          "  fstat -> 0%o (REG=shimmed-wrap-backing, CHR=native-tty)",
          st.st_mode & S_IFMT);

    /* isatty calls tcgetattr -> ioctl(TCGETS); our synth dispatch
     * forwards that to the real tty, so the call succeeds even
     * after the wrap. Unshimmed it's a real tty, also succeeds. */
    CHECK(isatty(s) == 1,
          "isatty after wrap -> %d (forwarded TCGETS sees tty)", isatty(s));

    close(m); close(s);
}

/* The ioctl shim now installs the wrap on every TCSETS (the previous
 * !ICANON gate was lifted), so a TCSETS that leaves ICANON set is a
 * new install path. The cache must be primed with canon=1 and reads
 * must keep falling through to the kernel's line discipline. */
static void test_install_in_canon(void)
{
    alarm(2);
    STATUS("--- install on canon-mode TCSETS keeps line cooking ---");
    int m, s;
    open_pty(&m, &s);

    struct termios t;
    if (ioctl(s, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    /* Default termios already has ICANON; ensure it's set and toggle
     * an unrelated flag so the TCSETS has a real effect. */
    t.c_lflag |= ICANON;
    t.c_lflag ^= ECHO;
    if (ioctl(s, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS canon");

    /* Canonical: kernel returns on newline. */
    if (write(m, "hi\n", 3) != 3) err(EXIT_FAILURE, "write");

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);

    CHECK(n == 3 && ms < 200,
          "canon read after install -> %zd bytes in %ldms", n, ms);
    CHECK(memcmp(buf, "hi\n", 3) == 0, "  contents 'hi\\n'");

    /* Switch to raw -- cache (canon -> 0, vmin/vtime) must update via
     * tty_ioctl, exercising the post-install update path from a
     * canon-primed initial state. */
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = 2;
    t.c_cc[VTIME] = 0;
    if (ioctl(s, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS raw");

    if (write(m, "ab", 2) != 2) err(EXIT_FAILURE, "write");
    usleep(50000);
    gettimeofday(&t0, NULL);
    n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    ms = elapsed_ms(&t0, &t1);
    CHECK(n == 2 && ms < 200,
          "raw VMIN=2 after canon install -> %zd in %ldms", n, ms);

    close(m); close(s);
}

/* tty_ioctl caches VMIN/VTIME on every TCSETS so tty_read can skip
 * TCGETS on the hot path. A TCSETS that only changes VMIN (keeping
 * canon=0) must refresh the cache -- if it didn't, the read would
 * use the stale VMIN and either return too early or block too long. */
static void test_vmin_only_update(void)
{
    alarm(2);
    STATUS("--- VMIN update without canon change refreshes cache ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 1, 0);                 /* install + cache VMIN=1 */

    /* Sanity: VMIN=1 returns the byte as soon as it arrives. */
    if (write(m, "a", 1) != 1) err(EXIT_FAILURE, "write");
    usleep(50000);
    char buf[16];
    ssize_t n = read(s, buf, sizeof(buf));
    CHECK(n == 1, "initial VMIN=1 -> %zd", n);

    /* Raise VMIN to 5 -- still raw, only c_cc[VMIN] changes. The
     * tty_ioctl observer must update the cached vmin atomic. */
    set_raw(s, 5, 0);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        usleep(300000);
        write(m, "hello", 5);
        _exit(0);
    }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    waitpid(pid, NULL, 0);

    CHECK(n >= 5 && ms >= 200,
          "VMIN=5 after update -> %zd in %ldms (want >=5 bytes, >=200ms)",
          n, ms);

    close(m); close(s);
}

/* tcsetattr() is the POSIX wrapper most apps actually use; our shim
 * forwards it to ioctl(TCSETS/W/F) which then goes through the
 * install path. Verify end-to-end so the wrapper integration is
 * exercised by something beyond direct ioctl() calls. */
static void test_tcsetattr_wrapper(void)
{
    alarm(2);
    STATUS("--- tcsetattr() drives the install + cache plumbing ---");
    int m, s;
    open_pty(&m, &s);

    struct termios t;
    CHECK(tcgetattr(s, &t) == 0, "tcgetattr -> ok");
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = 2;
    t.c_cc[VTIME] = 0;
    CHECK(tcsetattr(s, TCSANOW, &t) == 0, "tcsetattr(TCSANOW) raw VMIN=2");

    /* Round-trip via tcgetattr to verify state is what we set. */
    struct termios chk;
    CHECK(tcgetattr(s, &chk) == 0, "tcgetattr post-set -> ok");
    CHECK(!(chk.c_lflag & ICANON), "  ICANON cleared");
    CHECK(chk.c_cc[VMIN] == 2,
          "  VMIN=2 (got %u)", chk.c_cc[VMIN]);

    /* Functional: VMIN=2 returns once 2 bytes are available. */
    if (write(m, "ab", 2) != 2) err(EXIT_FAILURE, "write");
    usleep(50000);
    char buf[16];
    ssize_t n = read(s, buf, sizeof(buf));
    CHECK(n == 2, "VMIN=2 read after tcsetattr -> %zd", n);

    close(m); close(s);
}

/* TCSETSW (drain) and TCSETSF (flush) hit the same case-labels in
 * our shim as TCSETS, but the kernel does different things first.
 * Verify both end-to-end. The *2 variants (TCSETS2/W2/F2) take
 * struct termios2 (kernel-only layout, not in glibc <termios.h>) so
 * we skip them here; the case-labels are sibling fallthroughs of the
 * same observer code path. */
static void test_tcsets_variants(void)
{
    alarm(2);
    STATUS("--- TCSETSW and TCSETSF behave like TCSETS for install ---");

    struct {
        unsigned long op;
        const char    *name;
    } ops[] = {
        { TCSETSW, "TCSETSW" },
        { TCSETSF, "TCSETSF" },
    };

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        int m, s;
        open_pty(&m, &s);

        struct termios t;
        if (ioctl(s, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
        t.c_lflag &= ~ICANON;
        t.c_cc[VMIN]  = 3;
        t.c_cc[VTIME] = 0;
        CHECK(ioctl(s, ops[i].op, &t) == 0,
              "%s -> raw, VMIN=3", ops[i].name);

        if (write(m, "xyz", 3) != 3) err(EXIT_FAILURE, "write");
        usleep(50000);
        char buf[16];
        ssize_t n = read(s, buf, sizeof(buf));
        CHECK(n == 3, "  read after %s -> %zd (expected 3)", ops[i].name, n);

        close(m); close(s);
    }
}

/* tty.c bounds the tty_state table at TTY_SLOTS=64. The 65th install
 * silently returns -1 -- the kernel still honored TCSETS, just no
 * shim wrap. We can't distinguish "no wrap" from "wrap" via userland
 * observation alone (the WSL1 kernel may or may not honor VMIN on a
 * raw tty), so this is mostly a smoke test: we open enough PTYs to
 * exceed the table and verify nothing crashes and the early wraps
 * are still functional. If we can't open that many due to rlimit,
 * we skip. */
#define EXHAUST_N 70
static void test_install_table_full(void)
{
    alarm(2);
    STATUS("--- exhaust TTY_SLOTS, first wraps still work ---");

    int m[EXHAUST_N], s[EXHAUST_N];
    int n = 0;
    for (int i = 0; i < EXHAUST_N; i++) {
        int mfd = posix_openpt(O_RDWR | O_NOCTTY);
        if (mfd < 0) break;
        if (grantpt(mfd) < 0 || unlockpt(mfd) < 0) { close(mfd); break; }
        char *name = ptsname(mfd);
        if (!name) { close(mfd); break; }
        int sfd = open(name, O_RDWR | O_NOCTTY);
        if (sfd < 0) { close(mfd); break; }
        m[n] = mfd;
        s[n] = sfd;
        set_raw(s[n], 1, 0);          /* install attempt */
        n++;
    }

    if (n < 65) {
        STATUS("  SKIP: only opened %d PTYs (need >=65 to exhaust)", n);
    } else {
        CHECK(n >= 65, "opened %d PTYs (exhausts TTY_SLOTS=64)", n);

        /* First wrap is in-slot. Verify it's still functional. */
        if (write(m[0], "x", 1) != 1) err(EXIT_FAILURE, "write");
        usleep(50000);
        char buf[16];
        ssize_t r = read(s[0], buf, sizeof(buf));
        CHECK(r == 1, "  first wrap still functional (read %zd)", r);
    }

    for (int i = 0; i < n; i++) {
        close(m[i]); close(s[i]);
    }
}

/* tty_ops has no write/lseek callbacks; those ops must forward to the
 * real backing fd via the passthru path. write on the wrapped slave
 * should reach the master end; lseek should get ESPIPE from the
 * kernel (real ttys aren't seekable). Locks in the NULL->passthru
 * contract for I/O ops the handler doesn't override. */
static void test_passthru_io(void)
{
    alarm(2);
    STATUS("--- write/lseek on wrapped tty forward via passthru ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);    /* installs wrap */

    const char msg[] = "hello";
    ssize_t w = write(s, msg, sizeof(msg) - 1);
    CHECK(w == sizeof(msg) - 1,
          "write(wrapped slave, %zu) -> %zd", sizeof(msg) - 1, w);

    /* Bytes the slave wrote should land at the master end. */
    usleep(50000);
    char buf[16] = {0};
    ssize_t r = read(m, buf, sizeof(buf));
    CHECK(r == (ssize_t)(sizeof(msg) - 1),
          "  master read %zd of %zu bytes", r, sizeof(msg) - 1);
    CHECK(memcmp(buf, msg, sizeof(msg) - 1) == 0,
          "  bytes match (%.*s)", (int)r, buf);

    /* lseek on a tty (real or wrapped) should return ESPIPE from
     * the kernel via passthru forwarding. */
    errno = 0;
    off_t off = lseek(s, 0, SEEK_SET);
    CHECK(off == (off_t)-1 && errno == ESPIPE,
          "lseek on wrapped tty -> %ld errno=%d (expected -1/ESPIPE)",
          (long)off, errno);

    close(m); close(s);
}

/* F_SETFL on an O_PATH fd clobbers the O_PATH bit, which would
 * break the EBADF canary for subsequent ioctls on that fd. The
 * fcntl shim must dispatch BEFORE calling sym_next so the synth
 * O_PATH never sees the kernel-level F_SETFL. Without that fix,
 * the next TCSETS returns EPERM (kernel no longer recognizes the
 * fd as O_PATH) instead of routing through our wrap. */
static void test_setfl_preserves_wrap(void)
{
    alarm(2);
    STATUS("--- F_SETFL on wrapped fd must not break canary ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    /* Mimic the failing app: clear all status flags. */
    int r = fcntl(s, F_SETFL, 0);
    CHECK(r == 0, "fcntl(wrapped, F_SETFL, 0) -> %d", r);

    /* The wrap must still be alive: a follow-up TCSETS should
     * dispatch through us, not return EPERM from the kernel. */
    struct termios t;
    if (ioctl(s, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag |= ICANON;
    errno = 0;
    r = ioctl(s, TCSETS, &t);
    CHECK(r == 0,
          "ioctl(wrapped, TCSETS) -> %d errno=%d (expected 0)", r, errno);

    close(m); close(s);
}

/* FD_CLOEXEC set on the slave fd before TCSETS must survive the wrap
 * install. The naive dup2(synth, fd) would clear it, silently revoking
 * the caller's close-on-exec intent; we use dup3 with the right flag. */
static void test_install_preserves_cloexec(void)
{
    alarm(2);
    STATUS("--- FD_CLOEXEC set before wrap survives install ---");
    int m, s;
    open_pty(&m, &s);

    if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1) err(EXIT_FAILURE, "F_SETFD");
    int pre = fcntl(s, F_GETFD);
    CHECK(pre >= 0 && (pre & FD_CLOEXEC),
          "pre-install: FD_CLOEXEC set (got 0x%x)", pre);

    set_raw(s, 0, 0);    /* triggers tty_install */

    int post = fcntl(s, F_GETFD);
    CHECK(post >= 0 && (post & FD_CLOEXEC),
          "post-install: FD_CLOEXEC preserved (got 0x%x)", post);

    /* Negative case: fd without FD_CLOEXEC must stay that way. */
    int m2, s2;
    open_pty(&m2, &s2);
    set_raw(s2, 0, 0);
    int post2 = fcntl(s2, F_GETFD);
    CHECK(post2 >= 0 && !(post2 & FD_CLOEXEC),
          "default fd: FD_CLOEXEC stays cleared (got 0x%x)", post2);

    close(m); close(s); close(m2); close(s2);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: tty_wrap_test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* SIGALRM is a process-wide guard; each test_*() sets its own
     * per-test alarm() at entry so a hang surfaces within seconds
     * of the actual stuck call, rather than after a suite-wide budget. */
    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    test_canon_toggle();
    test_dup_wrapped();
    test_multiple_wraps();
    test_fork_inherits_wrap();
    test_visibility_quirks();
    test_install_in_canon();
    test_vmin_only_update();
    test_tcsetattr_wrapper();
    test_tcsets_variants();
    test_install_table_full();
    test_passthru_io();
    test_setfl_preserves_wrap();
    test_install_preserves_cloexec();

    TEST_REPORT();
}
