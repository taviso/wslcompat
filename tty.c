#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include "logging.h"
#include "shim.h"
#include "synth.h"
#include "tunables.h"

SHIM_INIT(read, ioctl, fcntl, close, poll);

struct tty_state {
    atomic_int      claimed;  // 0 = slot is free, 1 = in use
    atomic_int      canon;    // 1 = canonical mode, 0 = raw
    atomic_int      nonblock; // mirror of OFD's O_NONBLOCK, kept by tty_fcntl
    atomic_int      real_fd;  // duped tty fd; the synth fd takes the original.
    // CoW slots: a vfork-style child that relocates real_fd claims one
    // of these (pid + starttime + fd) instead of mutating the installer's
    // real_fd in shared memory.
    struct synth_override real_overrides[SYNTH_OVERRIDE_SLOTS];
    _Atomic cc_t    vmin;     // cached c_cc[VMIN], maintained by tty_ioctl
    _Atomic cc_t    vtime;    // cached c_cc[VTIME], same maintenance
};


// CoW accessors for real_fd via the generic view helpers in synth.c.
static int tty_real_fd_load(struct tty_state *s, struct synth_entry *e)
{
    return wslcompat_view_fd_load(e, s->real_overrides,
                                  _countof(s->real_overrides),
                                  &s->real_fd);
}


// Per-tty state stashed in synth_entry::userptr. tty_ioctl keeps
// `canon` in sync with TCSETS so tty_read can fast-path passthru in
// canonical mode without a TCGETS per read.
static struct tty_state tty_table[64];

static struct tty_state *tty_alloc(void)
{
    // Find and claim a slot in the state table.
    for (size_t i = 0; i < _countof(tty_table); i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&tty_table[i].claimed, &expected, 1))
            return &tty_table[i];
    }

    return NULL;
}

// VMIN/VTIME wait per termios(3) "Canonical and noncanonical mode".
static int tty_read_raw_wait(int fd, cc_t vmin, cc_t vtime)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    // vmin  > 0: block indefinitely until at least one byte is available.
    // vmin == 0: block for vtime deciseconds (0 = polling read).
    int timeout = (vmin > 0) ? -1 : (vtime * 100);

    return sym_next(poll, &pfd, 1, timeout);
}

// Cooked-read VMIN/VTIME polling on the real tty.
static ssize_t tty_read(struct synth_entry *e, void *buf, size_t count)
{
    struct tty_state *s = e->userptr;
    int fd = tty_real_fd_load(s, e);
    cc_t vmin, vtime;
    int res;

    // Optional passthru.
    if (wslcompat_passthru("ioctl"))
        return sym_next(read, fd, buf, count);

    // Fast path: TCSETS says canonical - just let the kernel cook.
    if (atomic_load(&s->canon) || atomic_load(&s->nonblock)) {
        wsldbg("fd %d canonical or nonblocking, passthru", fd);
        return sym_next(read, fd, buf, count);
    }

    if (count == 0) {
        wsldbg("fd %d zero sized read, return immediately", fd);
        return 0;
    }

    // Cache maintained by tty_ioctl -- no TCGETS on the hot path.
    // Stale only if the app drives TCSETS via raw syscall.
    vmin  = atomic_load(&s->vmin);
    vtime = atomic_load(&s->vtime);

    // Check if data is ready.
    if ((res = tty_read_raw_wait(fd, vmin, vtime)) <= 0)
        return res;

    ssize_t n = sym_next(read, fd, buf, count);

    if (n <= 0 || vmin == 0 || n >= vmin || n >= count)
        return n;

    // Case 1 & 2: VMIN > 0, and we need more bytes.
    size_t total = n;
    char *ptr = (char *)buf + n;
    int timeout = vtime ? (vtime * 100) : -1;

    while (total < vmin && total < count) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (sym_next(poll, &pfd, 1, timeout) <= 0)
            break;
        if ((n = sym_next(read, fd, ptr, count - total)) <= 0)
            break;
        total += n;
        ptr   += n;
    }
    return total;
}

// Forward ioctl onto the real tty, then observe TCSETS-family ops to
// keep the canon hint in sync. The synth dispatch already pinned the
// entry so s is stable for the duration.
static int tty_ioctl(struct synth_entry *e, unsigned long op, void *arg)
{
    struct tty_state *s = e->userptr;
    int r = syscall(SYS_ioctl, tty_real_fd_load(s, e), op, arg);

    if (r == 0) {
        switch (op) {
            case TCSETS:
            case TCSETSW:
            case TCSETSF:
            case TCSETS2:
            case TCSETSW2:
            case TCSETSF2: {
                struct termios *t = arg;
                atomic_store(&s->canon, !!(t->c_lflag & ICANON));
                atomic_store(&s->vmin,  t->c_cc[VMIN]);
                atomic_store(&s->vtime, t->c_cc[VTIME]);
                break;
            }
        }
    }
    return r;
}

// Final-ref destructor. real_fd is our private dup so close goes via
// sym_next.
static void tty_release(struct synth_entry *e)
{
    struct tty_state *s = e->userptr;

    sym_next(close, atomic_load(&s->real_fd));
    atomic_store(&s->real_fd, -1);
    wslcompat_override_init(s->real_overrides, _countof(s->real_overrides));
    atomic_store(&s->claimed, 0);
}

// Forward fcntl to the real tty -- O_NONBLOCK probes/toggles must
// hit the backing, not the synth O_PATH. Mirror successful state
// changes into our caches so tty_read can stay off F_GETFL.
static int tty_fcntl(struct synth_entry *e, int cmd, void *arg)
{
    struct tty_state *s = e->userptr;
    int r = sym_next(fcntl, tty_real_fd_load(s, e), cmd, arg);

    if (r < 0)
        return r;

    switch (cmd) {
        case F_SETFL:
            atomic_store(&s->nonblock, !!((intptr_t)arg & O_NONBLOCK));
            break;
    }
    return r;
}

// Expose the duped real tty as the backing fd. real_fd serves all
// kinds: poll/select wait, NULL I/O forward, fstat passthrough (so
// S_ISCHR sees a char device), and preexec restoration.
static int tty_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    struct tty_state *s = e->userptr;
    (void)kind;
    return tty_real_fd_load(s, e);
}

// Caller is about to close/dup-over targetfd; rescue our real_fd if it
// matches so close-all loops don't kill the backing tty.
static int tty_relocate(struct synth_entry *e, int targetfd)
{
    struct tty_state *s = e->userptr;
    int new_fd = wslcompat_view_fd_relocate(e, s->real_overrides,
                                            _countof(s->real_overrides),
                                            &s->real_fd, targetfd);
    if (new_fd > 0) {
        wsllog("moved real_fd %d -> %d (pid=%d, installer=%d)",
               targetfd, new_fd, getpid(), e->install_pid);
        return 1;
    }
    if (new_fd < 0)
        wslwarn("could not relocate real_fd=%d: %m", targetfd);
    return 0;
}

static int tty_describe(struct synth_entry *e, char *buf, size_t size)
{
    struct tty_state *s = e->userptr;
    int real = tty_real_fd_load(s, e);
    char name[64];

    if (real < 0 || ttyname_r(real, name, sizeof(name)) != 0)
        return snprintf(buf, size, "tty wrap (real_fd=%d)", real);

    return snprintf(buf, size, "tty wrap for %s", name);
}

static const struct synth_ops tty_ops = {
    .read     = tty_read,
    .ioctl    = tty_ioctl,
    .fcntl    = tty_fcntl,
    .release  = tty_release,
    .passthru = tty_passthru,
    .relocate = tty_relocate,
    .describe = tty_describe,
};

// Wrap fd: dup it, mint a synth O_PATH, dup2 the synth over fd.
// Returns 0 on success, -1 on failure (leaving fd intact). Once
// installed, read/ioctl on fd dispatch via synth -> tty_ops; the
// wrap stays in place even after the app returns to canon mode.
int wslcompat_tty_install(int fd, const struct termios *initial)
{
    struct tty_state *s;
    int real  = -1;
    int synth = -1;
    int flags;
    int fdflags;

    if ((s = tty_alloc()) == NULL) {
        wslerror("table full, cannot track");
        return -1;
    }

    // F_DUPFD_CLOEXEC so the dup doesn't leak across exec(); start
    // searching from a non-stdio number to avoid coincidentally
    // overlapping with future stdio reopens.
    if ((real = sym_next(fcntl, fd, F_DUPFD_CLOEXEC, 3)) < 0) {
        wslwarn("fd=%d: F_DUPFD_CLOEXEC failed: %m", fd);
        goto fail;
    }

    // O_NONBLOCK lives on the OFD; FD_CLOEXEC on the fd entry.
    if ((flags = sym_next(fcntl, fd, F_GETFL)) < 0) {
        wslwarn("fd=%d: F_GETFL failed: %m", fd);
        goto fail;
    }

    if ((fdflags = sym_next(fcntl, fd, F_GETFD)) < 0) {
        wslwarn("fd=%d: F_GETFD failed: %m", fd);
        goto fail;
    }

    atomic_store(&s->real_fd, real);
    wslcompat_override_init(s->real_overrides, _countof(s->real_overrides));

    // Prime the caches: termios from the ioctl shim's observation,
    // O_NONBLOCK from a one-shot F_GETFL.
    atomic_store(&s->canon, !!(initial->c_lflag & ICANON));
    atomic_store(&s->vmin,  initial->c_cc[VMIN]);
    atomic_store(&s->vtime, initial->c_cc[VTIME]);
    atomic_store(&s->nonblock, !!(flags & O_NONBLOCK));

    wsldbg("fd=%d flags=%d vmin=%d vtime=%d lflag=%x canon=%d",
           fd,
           flags,
           initial->c_cc[VMIN],
           initial->c_cc[VTIME],
           initial->c_lflag,
           initial->c_lflag & ICANON);

    if ((synth = wslcompat_synth_open(O_CLOEXEC, &tty_ops, s)) < 0) {
        wslwarn("fd=%d: synth_open failed: %m", fd);
        goto fail;
    }

    // Now replace the fd with the synthetic one. dup3 not dup2 so we
    // can carry fd's FD_CLOEXEC across (dup2 clears it unconditionally).
    if (dup3(synth, fd, (fdflags & FD_CLOEXEC) ? O_CLOEXEC : 0) < 0) {
        wslwarn("fd=%d: dup3(synth=%d) failed: %m", fd, synth);
        goto fail;
    }

    // Drop our synth fd -- the caller's fd holds the OFD via dup2.
    // close() through the shim keeps the probe machinery honest.
    close(synth);
    wsldbg("wrapped fd=%d (real=%d)", fd, real);
    return 0;

fail:
    // If synth registered, close() shim drives release(); else clean up by hand.
    if (synth >= 0) {
        close(synth);
    } else {
        if (real >= 0)
            sym_next(close, real);
        atomic_store(&s->claimed, 0);
    }
    return -1;
}
