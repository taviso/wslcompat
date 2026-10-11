#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "internal.h"
#include "logging.h"
#include "shim.h"
#include "synth.h"
#include "utils.h"
#include "tunables.h"

SHIM_INIT(fstat, fstat64);

// Type to uniquely identify a process. Note that we only store the low 32 bits
// of starttime, which should still be enough. The reason is we can use atomic
// reads/writes if we keep it <= 8 bytes.
struct process {
    uint32_t starttime;
    uint32_t pid;
};

// CoW override machinery for vfork/CLONE_VM. The installer owns the
// base fd value (probe_fd, handler's real_fd, ...); children that
// diverge claim a per-pid slot and write their own fd there.

// Parse /proc/<pid>/stat's field 22 (process start time in jiffies).
// We can't sscanf cleanly because comm (field 2) is parenthesised and
// may contain spaces. Locate it by the LAST ')', skip the single-char
// state field, then let strtoull walk through the 18 numeric fields
// between state and starttime.
static unsigned long long read_proc_starttime(const char *path)
{
    char buf[512] = {0};
    char *p;
    ssize_t n;
    int fd;

    if ((fd = syscall(SYS_open, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0)
        return 0;

    if ((n = syscall(SYS_read, fd, buf, sizeof(buf) - 1)) < 0)
        wsldbg("read %s => %zd, %m", path, n);

    syscall(SYS_close, fd);

    if (n <= 0)
        return 0;

    if ((p = strrchr(buf, ')')) == NULL)
        return 0;

    p++;

    // Skip the state field (single char after a space).
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;

    // Skip 18 numeric fields (ppid..itrealvalue); the 19th is starttime.
    for (int i = 0; i < 18; i++)
        strtoull(p, &p, 10);

    return strtoull(p, NULL, 10);
}

static unsigned long long wslcompat_self_starttime_pid(pid_t id)
{
    static _Atomic struct process cache;
    struct process n = { .pid = id };
    struct process c = atomic_load_explicit(&cache, memory_order_acquire);

    if (c.pid == id)
        return c.starttime;

    n.starttime = read_proc_starttime("/proc/self/stat");
    atomic_store_explicit(&cache, n, memory_order_release);
    return n.starttime;
}

static unsigned long long wslcompat_self_starttime(void)
{
    return wslcompat_self_starttime_pid(getpid());
}

// Lookup starttime for an arbitrary pid (for GC).
static uint32_t pid_starttime(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    return read_proc_starttime(path);
}

void wslcompat_override_init(struct synth_override *slots, size_t n)
{
    for (size_t i = 0; i < n; i++)
        atomic_store_explicit(&slots[i].pid, 0, memory_order_relaxed);
}

static void wslcompat_override_gc(struct synth_override *slots, size_t n)
{
    pid_t self = getpid();

    for (size_t i = 0; i < n; i++) {
        // Acquire ensures we see the starttime written before pid was published.
        pid_t p = atomic_load_explicit(&slots[i].pid, memory_order_acquire);
        if (p == 0 || p == (pid_t)-1)
            continue;

        unsigned long long sst = atomic_load_explicit(&slots[i].starttime, memory_order_relaxed);

        if (p == self) {
            // Ourselves: compare against the cached starttime, no /proc read.
            if (sst == wslcompat_self_starttime())
                continue;  // our live slot
            // else: a recycled pid's stale slot; free it below
        } else if (pid_starttime(p) == sst) {
            continue;  // a live foreign slot
        }

        wsldbg("GC freeing override slot %zu (pid=%d)", i, (int)p);
        atomic_compare_exchange_strong(&slots[i].pid, &p, 0);
    }
}

static struct synth_override *wslcompat_override_get_pid(struct synth_override *slots,
                                                         size_t n, pid_t id,
                                                         unsigned long long st)
{
    for (size_t i = 0; i < n; i++) {
        if (atomic_load_explicit(&slots[i].pid, memory_order_acquire) == id
            && atomic_load_explicit(&slots[i].starttime, memory_order_relaxed) == st)
            return &slots[i];
    }
    return NULL;
}

// Serializes find+insert in wslcompat_override_acquire_pid. acquire is
// only called by non-installers (fork/vfork children), and a vfork child
// is single-threaded, so it can never block on a reservation left by a
// suspended sibling thread. Multithreaded vfork is POSIX-undefined.
static atomic_flag synth_acquire_lock = ATOMIC_FLAG_INIT;

static struct synth_override *wslcompat_override_acquire_pid(struct synth_override *slots,
                                                             size_t n, pid_t id,
                                                             unsigned long long st,
                                                             int initial_fd)
{
    wslcompat_override_gc(slots, n);

    while (atomic_flag_test_and_set_explicit(&synth_acquire_lock,
                                             memory_order_acquire))
        __asm__ __volatile__("pause");

    struct synth_override *result = NULL;

    // Pass 1: check if we already have a slot.
    if ((result = wslcompat_override_get_pid(slots, n, id, st)) != NULL) {
        wsltrace("pid=%d found override slot %zu fd=%d", (int)id,
                 (size_t)(result - slots),
                 atomic_load_explicit(&result->fd, memory_order_relaxed));
        goto out;
    }

    // Pass 2: Claim a free slot via reservation (-1).
    for (size_t i = 0; i < n; i++) {
        pid_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&slots[i].pid, &expected, (pid_t)-1,
                                                     memory_order_acquire,
                                                     memory_order_relaxed)) {
            // We own the slot. Initialize payload then publish our PID.
            atomic_store_explicit(&slots[i].starttime, st, memory_order_relaxed);
            atomic_store_explicit(&slots[i].fd, initial_fd, memory_order_relaxed);
            atomic_store_explicit(&slots[i].pid, id, memory_order_release);
            wsltrace("pid=%d claimed override slot %zu fd=%d", (int)id, i, initial_fd);
            result = &slots[i];
            goto out;
        }
    }

out:
    atomic_flag_clear_explicit(&synth_acquire_lock, memory_order_release);
    return result;
}

// True iff we own the installer slot of e (the base fields are valid
// for us, not just one of the override slots).
static bool synth_is_installer(struct synth_entry *e, pid_t id,
                               unsigned long long st)
{
    return id == e->install_pid && st == e->install_starttime;
}

int wslcompat_view_fd_load(struct synth_entry *e,
                           struct synth_override *slots, size_t n,
                           atomic_int *base)
{
    pid_t id = getpid();
    unsigned long long st = wslcompat_self_starttime_pid(id);
    struct synth_override *s;

    if (synth_is_installer(e, id, st))
        return atomic_load(base);

    if ((s = wslcompat_override_get_pid(slots, n, id, st)))
        return atomic_load_explicit(&s->fd, memory_order_relaxed);
    return atomic_load(base);
}

void wslcompat_view_fd_store(struct synth_entry *e,
                             struct synth_override *slots, size_t n,
                             atomic_int *base, int new_fd)
{
    pid_t id = getpid();
    struct synth_override *s;
    unsigned long long st = wslcompat_self_starttime_pid(id);

    if (synth_is_installer(e, id, st)) {
        atomic_store(base, new_fd);
        return;
    }

    if ((s = wslcompat_override_acquire_pid(slots, n, id, st, new_fd)) == NULL) {
        wslwarn("override slots exhausted; fd update dropped");
        return;
    }

    atomic_store_explicit(&s->fd, new_fd, memory_order_relaxed);
}

// Atomically transition the view fd from `expected` to `desired`.
// Returns 1 iff this caller performed the transition.
static int wslcompat_view_fd_cas(struct synth_entry *e,
                                 struct synth_override *slots, size_t n,
                                 atomic_int *base, int expected, int desired)
{
    pid_t id = getpid();
    unsigned long long st = wslcompat_self_starttime_pid(id);

    if (synth_is_installer(e, id, st)) {
        int cur = expected;
        return atomic_compare_exchange_strong_explicit(base, &cur, desired,
                                                       memory_order_acq_rel,
                                                       memory_order_acquire);
    }

    struct synth_override *s = wslcompat_override_acquire_pid(slots, n, id,
                                                              st, expected);
    if (s == NULL) {
        wslwarn("override slots exhausted; relocation dropped");
        return 0;
    }

    int cur = expected;
    return atomic_compare_exchange_strong_explicit(&s->fd, &cur, desired,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

// Move the view fd off targetfd via F_DUPFD_CLOEXEC. Returns the new
// fd on success, 0 if nothing was relocated (mismatch, or a concurrent
// racer won), -1 if the dup failed (and the view is marked dead). A
// losing racer closes its duplicate so the OFD isn't pinned by a leak.
int wslcompat_view_fd_relocate(struct synth_entry *e,
                               struct synth_override *slots, size_t n,
                               atomic_int *base, int targetfd)
{
    if (wslcompat_view_fd_load(e, slots, n, base) != targetfd)
        return 0;

    int new_fd = syscall(SYS_fcntl, targetfd, F_DUPFD_CLOEXEC, 3);
    if (new_fd < 0) {
        wslcompat_view_fd_cas(e, slots, n, base, targetfd, -1);
        return -1;
    }

    if (wslcompat_view_fd_cas(e, slots, n, base, targetfd, new_fd)) {
        wsltrace("relocated fd %d -> %d (winner)", targetfd, new_fd);
        return new_fd;
    }

    wsltrace("relocate fd %d -> %d lost; closing", targetfd, new_fd);
    syscall(SYS_close, new_fd);
    return 0;
}

// One-line wrappers for probe_fd CoW access.
static int synth_probe_fd_load(struct synth_entry *e)
{
    return wslcompat_view_fd_load(e, e->probe_overrides,
                                  _countof(e->probe_overrides),
                                  &e->probe_fd);
}

static void synth_probe_fd_store(struct synth_entry *e, int new_fd)
{
    wslcompat_view_fd_store(e, e->probe_overrides,
                            _countof(e->probe_overrides),
                            &e->probe_fd, new_fd);
}

// Platform assumptions (these are wsl1 specific)
//   read/write on O_PATH fd       -> -1/EBADF
//   readv/writev on O_PATH fd     -> -1/EPERM
//   select with O_PATH in any set -> -1/EPERM (whole call fails)
//   poll on O_PATH fd             -> POLLIN spuriously
//   flock(LOCK_SH) on O_PATH      -> 0 (silent no-op real lock)
//   F_GETFL on O_PATH             -> NOT reliable (F_SETFL clobbers O_PATH)
//   /proc/self/fd/N reopen        -> fresh OFD on the same inode

// Slot lifecycle is encoded entirely in `refcount`:
//   0  : free, available for synth_register to claim
//  -1  : transient, register claimed the slot
//  >=1 : live, with 1 reference held by the producer (synth_open) plus
//        one per concurrent dispatch pin. The last decrementer (whose
//        CAS takes the count from 1 to -1) is the unique owner of cleanup and
//        closes the probe fd; everyone else just decrements and leaves.
//
// Pinning uses an atomic "increment-if-positive" loop. After a successful pin,
// dispatch must re-verify (dev, ino) -- a slot can have been freed and re-claimed
// for a different inode between the lookup decision and the pin.
static struct synth_entry synth_table[64];
static atomic_uint synth_live_entries;

// Increment refcount iff positive (live and not transient).
// Returns 1 if pinned, 0 if not pinnable.
static int synth_pin(struct synth_entry *e)
{
    int cur = atomic_load_explicit(&e->refcount, memory_order_relaxed);

    // If cur is negative or zero, there is nothing to claim.
    while (cur > 0) {
        // Atomically try refcount cur -> cur+1. On success we own a reference.
        if (atomic_compare_exchange_weak_explicit(&e->refcount,
                                                  &cur,
                                                  cur + 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            wsltrace("slot %ld pin: refcount %d -> %d", e - synth_table, cur, cur + 1);
            return 1;
        }
    }

    // Either the slot was free/transient when we first loaded, or it
    // transitioned to free/transient before any exchange could land.
    return 0;
}

// Drop the caller's reference.
// If our decrement takes the count from 1 to 0 we are the unique cleaner and
// transition through -1.
void wslcompat_synth_unpin(struct synth_entry *e)
{
    int cur = atomic_load_explicit(&e->refcount, memory_order_relaxed);

    while (cur > 0) {
        // If cur == 1, try to transition to -1 (CLEANUP) to lock out relocations.
        // Otherwise, just decrement.
        int next = (cur == 1) ? -1 : (cur - 1);

        if (atomic_compare_exchange_weak_explicit(
                &e->refcount, &cur, next,
                memory_order_acq_rel, memory_order_relaxed)) {
            if (next == -1) {
                // Unique cleaner. release() runs first (handler state
                // teardown while ops is still valid), then we close
                // the probe fd and publish the slot as free.
                if (e->ops->release)
                    e->ops->release(e);
                int probe = synth_probe_fd_load(e);
                if (probe != -1)
                    syscall(SYS_close, probe);
                synth_probe_fd_store(e, -1);
                atomic_store_explicit(&e->refcount, 0, memory_order_release);
                atomic_fetch_sub_explicit(&synth_live_entries, 1,
                                          memory_order_release);
            }
            return;
        }
    }
}

// Scan for a pinnable entry matching (dev, ino) in *key.
// Caller MUST synth_unpin the returned entry when done.
static struct synth_entry *synth_pin_lookup(const struct stat *key)
{
    wsltrace("lookup key dev=%lu ino=%lu", key->st_dev, key->st_ino);

    for (size_t i = 0; i < _countof(synth_table); i++) {
        struct synth_entry *e = &synth_table[i];

        if (!synth_pin(e))
            continue;

        wsltrace("  slot %zu has dev=%lu ino=%lu", i, e->stat.st_dev, e->stat.st_ino);

        if (e->stat.st_dev == key->st_dev && e->stat.st_ino == key->st_ino)
            return e;

        wslcompat_synth_unpin(e);
    }
    return NULL;
}

// Sweep slots under table-full pressure: run the normal close path over
// each entry, so any whose wrap has gone dead system-wide (its fork-child
// holder has since exited) is released and its slot freed; live entries
// defer, exactly as a real close would. Pinning hands post_close the
// reference it consumes. Returns true as soon as one slot comes free --
// the retry only needs one, so there's no point sweeping the rest.
static bool synth_table_gc(void)
{
    for (size_t i = 0; i < _countof(synth_table); i++) {
        struct synth_entry *e = &synth_table[i];
        if (!synth_pin(e))
            continue;
        wslcompat_synth_post_close(e);
        if (atomic_load_explicit(&e->refcount, memory_order_acquire) == 0)
            return true;
    }
    return false;
}

// Claim a free slot and publish state.
// Returns the freshly-published entry (refcount=1, producer's reference)
// or NULL if the table is full.
static struct synth_entry *synth_register(const struct stat *st,
                                          const struct synth_ops *ops,
                                          int probe_fd, void *userptr,
                                          int flags)
{
    for (size_t i = 0; i < _countof(synth_table); i++) {
        struct synth_entry *e = &synth_table[i];
        int expected = 0;

        // Claim 0 -> -1. Strong because spurious failure would skip a usable slot.
        if (!atomic_compare_exchange_strong_explicit(&e->refcount,
                                                     &expected, -1,
                                                     memory_order_acquire,
                                                     memory_order_relaxed)) {
            continue;
        }

        atomic_fetch_add_explicit(&synth_live_entries, 1,
                                  memory_order_release);

        e->stat              = *st;
        e->ops               = ops;
        e->probe_fd          = probe_fd;
        e->userptr           = userptr;
        e->flags             = flags;
        e->pos               = 0;
        e->install_pid       = getpid();
        e->install_starttime = wslcompat_self_starttime();
        wslcompat_override_init(e->probe_overrides,
                                _countof(e->probe_overrides));

        // Publish: RELEASE pairs with the ACQUIRE in synth_pin so the
        // field writes above are visible to any thread that observes
        // refcount as positive afterwards.
        atomic_store_explicit(&e->refcount, 1, memory_order_release);

        wsldbg("slot %zu register: dev=%lu ino=%lu probe_fd=%d",
               i, st->st_dev, st->st_ino, probe_fd);
        return e;
    }

    // Table full. A deferred entry may since have gone dead system-wide;
    // sweep, and retry only if that freed something (so we can't spin).
    if (synth_table_gc())
        return synth_register(st, ops, probe_fd, userptr, flags);
    return NULL;
}

// This creates a "probe" fd, which is a separate OFD created via /proc/self/fd
// to monitor for when all references to our synthetic fd have been closed.
//
// This exploits a wsl1 specific quirk - on real linux flock() on an O_PATH fd
// returns EBADF, but on WSL1 you can totally just flock() any garbage you want.
//
// This is good news for us - we can monitor when the last reference to a
// synthetic file has been closed without super fiddly dup()/clone()/etc
// book-keeping, the kernel does it all for us!
static int synth_install_probe(int caller_fd)
{
    char proc[64];
    int probe_fd;

    if (syscall(SYS_flock, caller_fd, LOCK_SH | LOCK_NB) != 0) {
        wsldbg("flock on O_PATH fd=%d unsupported here: %m", caller_fd);
        return -1;
    }

    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", caller_fd);

    if ((probe_fd = syscall(SYS_open, proc, O_RDONLY | O_CLOEXEC)) < 0) {
        wsldbg("could not open liveness probe via %s: %m", proc);
        syscall(SYS_flock, caller_fd, LOCK_UN);
        return -1;
    }

    return probe_fd;
}

// Move any internal fd matching targetfd out of the way before the
// caller's close/dup2/dup3 retires it. Two patterns motivate this:
// shell `exec 6< file.txt` (caller doesn't know we use fd 6), and
// close-all loops between fork and exec (gdb, fakeroot) walking
// upward over our internal fds.
//
// min_fd=3 keeps stdio out, and -- for close-all walking up -- lands
// us in already-processed territory below the loop's cursor.
//
// We MUST pin first; free slots retain stale probe_fd values.
void wslcompat_synth_relocate_internal(int targetfd)
{
    wsltrace("searching for %d", targetfd);

    for (size_t i = 0; i < _countof(synth_table); i++) {
        struct synth_entry *e = &synth_table[i];

        if (!synth_pin(e))
            continue;

        int new_fd = wslcompat_view_fd_relocate(e, e->probe_overrides,
                                                _countof(e->probe_overrides),
                                                &e->probe_fd, targetfd);
        if (new_fd > 0)
            wsllog("moved probe fd %d -> %d to avoid collision", targetfd, new_fd);
        else if (new_fd < 0)
            wslwarn("could not relocate probe at fd=%d: %m", targetfd);

        if (e->ops->relocate)
            e->ops->relocate(e, targetfd);

        wslcompat_synth_unpin(e);
    }
}

int wslcompat_synth_open(int flags, const struct synth_ops *ops, void *userptr)
{
    char proc[64];
    char title[256];
    struct stat st;
    int back_fd = -1;
    int path_fd = -1;
    int probe_fd = -1;
    int n;
    struct synth_entry *e;
    char label[128] = "no description";

    // Step 1: get a backing inode via the O_TMPFILE polyfill in open.c.
    // We deliberately go through the open shim so the polyfill kicks in.
    if ((back_fd = open("/tmp", O_RDWR | O_TMPFILE | O_CLOEXEC, 0600)) < 0) {
        wsldbg("could not create backing tmpfile: %m");
        goto fail;
    }

    // Step 2: re-open the same inode as O_PATH -- this is the fd we
    // hand back to the caller. Propagate O_CLOEXEC from the request
    // so the synthetic fd doesn't leak across exec().
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", back_fd);
    if ((path_fd = syscall(SYS_open, proc, O_PATH | (flags & O_CLOEXEC))) < 0) {
        wsldbg("could not re-open backing as O_PATH: %m");
        goto fail;
    }

    // Step 3: identify the inode for the (dev, ino) lookup key.
    if (syscall(SYS_fstat, path_fd, &st) != 0) {
        wsldbg("fstat on path_fd=%d failed: %m", path_fd);
        goto fail;
    }

    // Step 4: install the liveness probe. Best-effort -- a -1 probe_fd
    // just means we can't detect last-close on this kernel.
    probe_fd = synth_install_probe(path_fd);

    // Step 5: publish into the table.
    if ((e = synth_register(&st, ops, probe_fd, userptr, flags)) == NULL) {
        wslwarn("synth table full (cap=%ld), refusing new entry", _countof(synth_table));
        errno = ENFILE;
        goto fail;
    }

    // Step 6: best-effort debug banner via the still-open O_RDWR
    // back_fd, so `cat /proc/PID/fd/N` on the (deleted) tmpfile shows
    // what we know about this synth.
    if (ops->describe) {
        ops->describe(e, label, sizeof(label));
    }

    if ((n = snprintf(title, sizeof(title),
                     "synthetic fd: slot %ld/%ld (%s)\n",
                      e - synth_table,
                      _countof(synth_table),
                      label)) > 0) {
        syscall(SYS_write, back_fd, title, n);
    }

    // Step 7: drop the O_RDWR backing fd. The inode stays alive via
    // the O_PATH fd we hand back.
    syscall(SYS_close, back_fd);

    wsldbg("registered synthetic fd=%d (dev=%lu ino=%lu probe=%d)",
           path_fd, st.st_dev, st.st_ino, probe_fd);
    return path_fd;

fail:
    if (probe_fd != -1)
        syscall(SYS_close, probe_fd);
    if (path_fd != -1)
        syscall(SYS_close, path_fd);
    if (back_fd != -1)
        syscall(SYS_close, back_fd);
    return -1;
}

// Pin the entry for fd's (dev, ino), or NULL if not ours.
// Caller MUST wslcompat_synth_unpin when done.
struct synth_entry *wslcompat_synth_pin_fd(int fd)
{
    struct stat st;

    if (syscall(SYS_fstat, fd, &st) != 0)
        return NULL;

    return synth_pin_lookup(&st);
}

// fstat shim: for synth fds, override the result with the backing
// fd's stat. Apps that gate behavior on S_ISCHR (gdb's terminal
// detection, isatty alternatives) would otherwise see our tmpfile
// inode (S_IFREG) and skip tty-specific code paths. Forward the
// initial read via sym_next(fstat, ...) so it reports the tmpfile
// inode, keeping it consistent with synth_pin_fd's (dev, ino) key.
int fstat(int fd, struct stat *st)
{
    int real = -1;
    int r = sym_next(fstat, fd, st);

    wsltrace("fd=%d", fd);

    if (r != 0 || wslcompat_passthru_self() || !wslcompat_synth_active())
        return r;

    SYNTH_AUTOPIN_FD(e, fd);

    if (e == NULL)
        return r;

    if (e->ops->passthru)
        real = e->ops->passthru(e, SYNTH_OP_STAT);

    if (real >= 0) {
        wsltrace("synth fd=%d -> backing fd=%d", fd, real);
        return sym_next(fstat, real, st);
    }

    return 0;
}

int fstat64(int fd, struct stat64 *st)
{
    int real = -1;
    int r = sym_next(fstat64, fd, st);

    wsltrace("fd=%d", fd);

    if (r != 0 || wslcompat_passthru("fstat") || !wslcompat_synth_active())
        return r;

    SYNTH_AUTOPIN_FD(e, fd);

    if (e == NULL)
        return r;

    if (e->ops->passthru)
        real = e->ops->passthru(e, SYNTH_OP_STAT);

    if (real >= 0) {
        wsltrace("synth fd=%d -> backing fd=%d", fd, real);
        return sym_next(fstat64, real, st);
    }

    return 0;
}

// Resolve fd through the handler's passthru callback. Used by the
// poll/select shims (SYNTH_OP_POLL); the SYNTH_DISPATCH macro
// consults e->ops->passthru directly for SYNTH_OP_IO.
int wslcompat_synth_passthru(int fd, enum synth_op_kind kind)
{
    int r = -1;

    SYNTH_AUTOPIN_FD(e, fd);

    if (e == NULL)
        return -1;

    if (e->ops->passthru)
        r = e->ops->passthru(e, kind);

    wsltrace("synth fd=%d kind=%d => fd=%d", fd, kind, r);
    return r;
}

int wslcompat_synth_active(void)
{
    return atomic_load_explicit(&synth_live_entries,
                                memory_order_acquire) != 0;
}


// Pass-1 of preexec_save. Picks up user_fds whose stat
// matches a live synth entry; stashes them in the candidate array
// for pass-2 to swap. We don't mutate during the walk -- F_DUPFD in
// pass-2 would otherwise re-surface in a later getdents64 batch and
// trick pass-1 into "collecting" our own parking fds.
struct preexec_candidates {
    int fds[SYNTH_PREEXEC_MAX];
    size_t count;
};

static int preexec_collect_visit(int fd, void *user)
{
    struct preexec_candidates *c = user;
    struct synth_entry *e;

    if (c->count >= _countof(c->fds))
        return 1;

    if ((e = wslcompat_synth_pin_fd(fd)) == NULL)
        return 0;

    wslcompat_synth_unpin(e);

    c->fds[c->count++] = fd;
    return 0;
}

// Park user_fd's synth OFD at a high CLOEXEC fd and dup the handler's
// SYNTH_OP_IO fd over user_fd. The parking fd keeps a 2nd ref on the
// synth OFD, so post_close's flock probe sees the LOCK_SH still held
// and the entry survives the dup3 swap. FD_CLOEXEC on user_fd is
// preserved so adddup2-style laundering of a CLOEXEC wrap still
// surfaces real_fd in the child, not the synth O_PATH.
static void preexec_save_one(int user_fd, struct synth_preexec_state *st)
{
    struct synth_entry *e = wslcompat_synth_pin_fd(user_fd);
    int real_fd = -1;
    int cloexec = 0;
    int fdflags;
    int saved;

    if (e == NULL)
        return;

    if (e->ops->passthru)
        real_fd = e->ops->passthru(e, SYNTH_OP_PREEXEC);

    wslcompat_synth_unpin(e);

    if (real_fd < 0)
        return;

    fdflags = syscall(SYS_fcntl, user_fd, F_GETFD);

    if (fdflags >= 0 && (fdflags & FD_CLOEXEC))
        cloexec = 1;

    if ((saved = syscall(SYS_fcntl, user_fd, F_DUPFD_CLOEXEC, _countof(synth_table))) < 0) {
        wslwarn("preexec save: F_DUPFD_CLOEXEC of fd=%d failed: %m", user_fd);
        return;
    }

    // dup3 via the shim: implicit close-of-newfd must fire pre/post_close.
    if (dup3(real_fd, user_fd, cloexec ? O_CLOEXEC : 0) < 0) {
        wslwarn("preexec save: dup3(real=%d, user=%d) failed: %m",
                real_fd, user_fd);
        syscall(SYS_close, saved);
        return;
    }

    st->slots[st->count].user_fd      = user_fd;
    st->slots[st->count].saved_fd     = saved;
    st->slots[st->count].orig_cloexec = cloexec;
    st->count++;

    wsldbg("preexec save: user=%d parked at %d (real=%d cloexec=%d)",
           user_fd, saved, real_fd, cloexec);
}

void wslcompat_synth_preexec_save(struct synth_preexec_state *st)
{
    struct preexec_candidates c = { .count = 0 };

    wslcompat_walk_proc_self_fd(preexec_collect_visit, &c);

    for (size_t i = 0; i < c.count; i++) {
        if (st->count >= _countof(st->slots))
            break;
        preexec_save_one(c.fds[i], st);
    }
}

void wslcompat_synth_preexec_restore(struct synth_preexec_state *st)
{
    for (size_t i = 0; i < st->count; i++) {
        int user_fd  = st->slots[i].user_fd;
        int saved_fd = st->slots[i].saved_fd;
        int cloexec  = st->slots[i].orig_cloexec;

        // dup3 via the shim: user_fd is currently real_fd's OFD, which
        // isn't ours, so pre/post_close are a no-op on user_fd.
        if (dup3(saved_fd, user_fd, cloexec ? O_CLOEXEC : 0) < 0) {
            wslwarn("preexec restore: dup3(saved=%d, user=%d) failed: %m",
                    saved_fd, user_fd);
        } else {
            wsldbg("preexec restore: synth back at user=%d", user_fd);
        }

        // close via the shim: saved_fd is the synth OFD, so this fires
        // pre/post_close. user_fd still holds the OFD so the probe
        // fails and the entry stays alive.
        close(saved_fd);
    }
    st->count = 0;
}

// Pin fd's entry if it's ours, ahead of a close-like op.
// Caller MUST pair with wslcompat_synth_post_close.
struct synth_entry *wslcompat_synth_pre_close(int fd)
{
    return wslcompat_synth_pin_fd(fd);
}

// Visitor: stops the walk on the first fd (other than our probe) that
// points to the wrap's inode -> some other fd in this process still
// references the wrap.
struct post_close_scan {
    struct synth_entry *e;
    int   probe_fd;
    bool  found_other;
};

static int post_close_visit(int fd, void *user)
{
    struct post_close_scan *c = user;
    struct stat st;

    if (fd == c->probe_fd)
        return 0;
    if (syscall(SYS_fstat, fd, &st) != 0)
        return 0;
    if (st.st_dev != c->e->stat.st_dev || st.st_ino != c->e->stat.st_ino)
        return 0;

    c->found_other = true;
    return 1;
}

// "Is this process done with the wrap?" -- decides whether post_close
// should drop the producer ref and tear down handler state (real_fd,
// override slots, etc).
//
// Fast path: LOCK_EX on probe_fd succeeds iff no LOCK_SH holders exist
// system-wide. System-wide dead implies process-local dead, so release.
//
// Slow path (lock contested): some fd somewhere holds the inode. Walk
// /proc/self/fd to tell apart "another fd in *this* process holds it
// (dup, defer)" from "only a fork child or unrelated process holds it
// (safe to drop our handler state)".
//
// The walk is important for the pty-master-POLLHUP case: parent open()'s a
// slave, tcsetattr() makes the wrap, fork(), parent close()'s its wrap copy
// while the child still pauses holding its inherited copy. Without the walk,
// parent's hidden real_fd dup of the slave stays alive and the master never
// sees POLLHUP. asyncio, emerge, etc. depend on this firing.
static bool synth_should_release(struct synth_entry *e, int probe_fd)
{
    if (probe_fd != -1
        && syscall(SYS_flock, probe_fd, LOCK_EX | LOCK_NB) == 0)
        return true;

    struct post_close_scan c = {
        .e = e, .probe_fd = probe_fd, .found_other = false,
    };
    wslcompat_walk_proc_self_fd(post_close_visit, &c);
    if (c.found_other)
        return false;
    return true;
}

void wslcompat_synth_post_close(struct synth_entry *e)
{
    if (e == NULL)
        return;

    // Only the installer process may release. Non-installer descendants
    // (fork children and -- critically -- vfork children sharing memory)
    // must not mutate the entry: vfork's CLONE_VM means tty_release in
    // the child would corrupt the parent's view of real_fd. Descendants'
    // fd refs are cleaned up by the kernel on process exit.
    long idx = e - synth_table;
    pid_t self = getpid();
    unsigned long long self_st = wslcompat_self_starttime();
    if (e->install_pid != self || e->install_starttime != self_st) {
        wsltrace("slot %ld post_close: non-installer (pid=%d), defer to kernel",
                 idx, self);
        wslcompat_synth_unpin(e);
        return;
    }

    int probe_fd = synth_probe_fd_load(e);
    if (synth_should_release(e, probe_fd)) {
        wsldbg("slot %ld post_close: no local refs, dropping producer", idx);
        wslcompat_synth_unpin(e);  // producer ref
    } else {
        wsltrace("slot %ld post_close: local dup still holds wrap, defer",
                 idx);
    }

    wslcompat_synth_unpin(e);  // pre_close pin
}

