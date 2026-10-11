#ifndef WSLCOMPAT_SYNTH_H
#define WSLCOMPAT_SYNTH_H

#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "shim.h"
#include "tunables.h"

// Per-pid CoW slot for an fd. The base value (e.g. probe_fd, real_fd)
// is the parent's; a vfork-style child that needs to diverge claims
// one of these slots with its own (pid, starttime) identity and
// writes its own fd value. Reads compose: try our slot, fall back to
// base. starttime distinguishes the live process from a recycled pid.
struct synth_override {
    _Atomic pid_t               pid;        // 0 == free
    _Atomic unsigned long long  starttime;
    atomic_int                  fd;         // handler-interpreted
};

#define SYNTH_OVERRIDE_SLOTS 4

// Per-entry record for a synthesized fd.
struct synth_ops;
struct synth_entry {
    atomic_int refcount;
    atomic_int probe_fd;
    struct stat stat;
    const struct synth_ops *ops;
    // Handler can do whatever it wants with this.
    _Atomic off_t pos;
    // Opaque per-handler slot from wslcompat_synth_open's `userptr`.
    void *userptr;
    // Open flags the caller passed to wslcompat_synth_open. Stored
    // because once the wrap is an O_PATH fd, F_GETFL returns O_PATH and
    // the original O_NONBLOCK/O_APPEND/access mode are unrecoverable.
    int flags;

    // Installer identity. probe_fd / handler base fds are valid only
    // for this (pid, starttime); other processes use override slots.
    pid_t              install_pid;
    unsigned long long install_starttime;

    // Per-pid CoW slots for probe_fd. Handlers that own their own
    // fd (tty's real_fd) keep a parallel array in their own state.
    struct synth_override probe_overrides[SYNTH_OVERRIDE_SLOTS];
};

// Per-handler op table.
typedef ssize_t (*synth_read_fn)(struct synth_entry *e,
                                 void *buf, size_t count);
typedef ssize_t (*synth_write_fn)(struct synth_entry *e,
                                  const void *buf, size_t count);
typedef ssize_t (*synth_pread_fn)(struct synth_entry *e,
                                  void *buf, size_t count, off_t offset);
typedef ssize_t (*synth_pwrite_fn)(struct synth_entry *e,
                                   const void *buf, size_t count, off_t offset);
typedef ssize_t (*synth_readv_fn)(struct synth_entry *e,
                                  const struct iovec *iov, int iovcnt);
typedef ssize_t (*synth_writev_fn)(struct synth_entry *e,
                                   const struct iovec *iov, int iovcnt);
typedef ssize_t (*synth_preadv_fn)(struct synth_entry *e,
                                   const struct iovec *iov, int iovcnt, off_t offset);
typedef ssize_t (*synth_pwritev_fn)(struct synth_entry *e,
                                    const struct iovec *iov, int iovcnt, off_t offset);
typedef off_t   (*synth_lseek_fn)(struct synth_entry *e,
                                  off_t offset, int whence);
typedef void *  (*synth_mmap_fn)(struct synth_entry *e,
                                 void *addr, size_t length, int prot,
                                 int flags, int fd, off_t offset);
typedef int     (*synth_ioctl_fn)(struct synth_entry *e,
                                  unsigned long op, void *arg);
typedef int     (*synth_fcntl_fn)(struct synth_entry *e,
                                  int cmd, void *arg);
typedef int     (*synth_fsync_fn)(struct synth_entry *e);
typedef int     (*synth_fdatasync_fn)(struct synth_entry *e);
typedef int     (*synth_ftruncate_fn)(struct synth_entry *e, off_t length);
typedef int     (*synth_fchmod_fn)(struct synth_entry *e, mode_t mode);
typedef int     (*synth_fchown_fn)(struct synth_entry *e,
                                   uid_t owner, gid_t group);
typedef int     (*synth_fallocate_fn)(struct synth_entry *e,
                                      int mode, off_t offset, off_t len);
typedef ssize_t (*synth_fgetxattr_fn)(struct synth_entry *e,
                                      const char *name, void *value, size_t size);
typedef int     (*synth_fsetxattr_fn)(struct synth_entry *e,
                                      const char *name, const void *value,
                                      size_t size, int flags);
typedef ssize_t (*synth_flistxattr_fn)(struct synth_entry *e,
                                       char *list, size_t size);
typedef int     (*synth_fremovexattr_fn)(struct synth_entry *e,
                                         const char *name);

// close: fired by the close() shim whenever a fd referencing the entry
//        is being closed. May fire multiple times during the entry's
//        lifetime if the caller dup'd before close. DO NOT release
//        final per-entry state here -- another fd may still reference
//        the OFD; use `release` for that.
// dup:   fired by the dup2()/dup3() shim when the synth fd is the
//        newfd being implicitly closed by the dup. Same lifetime
//        semantics as `close` (may fire repeatedly, not the final
//        notification).
// release: fired exactly once, from synth.c, when the entry is
//          actually being torn down (probe detected the last OFD ref
//          has gone away). Release per-entry state -- close any
//          stashed fds, free state slots -- here.
typedef void    (*synth_close_fn)(struct synth_entry *e);
typedef void    (*synth_dup_fn)(struct synth_entry *e);
typedef void    (*synth_release_fn)(struct synth_entry *e);

// relocate: if targetfd is one of the handler's internal fds, move it
// out of the way via F_DUPFD_CLOEXEC. Returns 1 if relocated.
typedef int     (*synth_relocate_fn)(struct synth_entry *e, int targetfd);

// describe: human-readable label written into the O_TMPFILE backing at
// open time, so `cat /proc/PID/fd/N` on the (deleted) tmpfile reveals
// what kind of synth fd N actually is.
typedef int (*synth_describe_fn)(struct synth_entry *e,
                                 char *buf, size_t size);

// passthru: returns the underlying kernel fd this synth wraps.
//   - SYNTH_OP_POLL:    fd that poll/select shims wait on.
//   - SYNTH_OP_IO:      fd that NULL read/write/lseek/ioctl/fcntl
//                       callbacks are forwarded to.
//   - SYNTH_OP_STAT:    fd whose stat should be reported when an
//                       app fstats the synth (so S_ISCHR / S_ISFIFO
//                       checks see the real backing, not our O_PATH
//                       tmpfile).
//   - SYNTH_OP_PREEXEC: the caller-visible fd where this wrap lives,
//                       so the execve shim can dup the backing fd
//                       (SYNTH_OP_IO) back over it before exec --
//                       our state can't follow exec, so we restore
//                       the original kernel fd. Return -1 to opt out
//                       (the synth fd will simply be useless in the
//                       new image).
//   Return -1 from POLL/IO if there is no backing for that kind --
//   those ops then fail with EBADF (or whatever saved_errno was).
enum synth_op_kind {
    SYNTH_OP_POLL,
    SYNTH_OP_IO,
    SYNTH_OP_STAT,
    SYNTH_OP_PREEXEC,
};
typedef int     (*synth_passthru_fn)(struct synth_entry *e,
                                     enum synth_op_kind kind);

struct synth_ops {
    synth_read_fn      read;
    synth_write_fn     write;
    synth_pread_fn     pread;
    synth_pwrite_fn    pwrite;
    synth_readv_fn     readv;
    synth_writev_fn    writev;
    synth_preadv_fn    preadv;
    synth_pwritev_fn   pwritev;
    synth_lseek_fn     lseek;
    synth_mmap_fn      mmap;
    synth_ioctl_fn     ioctl;
    synth_fcntl_fn     fcntl;
    synth_fsync_fn     fsync;
    synth_fdatasync_fn fdatasync;
    synth_ftruncate_fn ftruncate;
    synth_fchmod_fn    fchmod;
    synth_fchown_fn    fchown;
    synth_fallocate_fn fallocate;
    synth_fgetxattr_fn  fgetxattr;
    synth_fsetxattr_fn  fsetxattr;
    synth_flistxattr_fn flistxattr;
    synth_fremovexattr_fn fremovexattr;
    synth_close_fn     close;
    synth_dup_fn       dup;
    synth_release_fn   release;
    synth_passthru_fn  passthru;
    synth_relocate_fn  relocate;
    synth_describe_fn  describe;
};

// Synthesize a fd backed by ops. Returns an O_PATH file descriptor on
// success (which the caller should hand back to the application) or -1
// on failure with errno set.
//
// The flags and userptr are stored in the synth_entry and available to handlers.
int wslcompat_synth_open(int flags, const struct synth_ops *ops, void *userptr);

// Pin the synth entry that owns fd, or NULL if fd isn't ours. Pinning
// holds the entry alive while the caller works with it; the caller
// MUST pair with wslcompat_synth_unpin. fstat is amortized against
// the failing syscall that triggered the lookup.
struct synth_entry *wslcompat_synth_pin_fd(int fd);
void                wslcompat_synth_unpin(struct synth_entry *e);

static inline void wslcompat_synth_unpin_cleanup(struct synth_entry **ep) {
    if (*ep) wslcompat_synth_unpin(*ep);
}

#define SYNTH_AUTOPIN_FD(var, fd) \
    __attribute__((cleanup(wslcompat_synth_unpin_cleanup))) \
    struct synth_entry *var = wslcompat_synth_pin_fd(fd)

// Dispatch an I/O op (read/write/lseek/ioctl/fcntl) that hit the
// kernel's EBADF canary. If fd resolves to a synth entry:
//   - if e->ops-><call> is set, runs it and RETURNS its result;
//   - else if e->ops->passthru returns a real fd, forwards via
//     sym_next(<call>, real, ...) and RETURNS that result;
//   - else falls through with errno preserved as it was on entry,
//     so the caller can return its original failed result.
//
// Usage:
//   ssize_t read(int fd, void *buf, size_t count) {
//       ssize_t r = sym_next(read, fd, buf, count);
//       if (r != -1 || errno != EBADF) return r;
//       SYNTH_DISPATCH(read, fd, buf, count);
//       return r;
//   }
#define SYNTH_DISPATCH(call, fd, ...) do {                              \
    int _saved = errno;                                                 \
    SYNTH_AUTOPIN_FD(_e, fd);                                           \
    if (_e == NULL) { errno = _saved; break; }                          \
    if (_e->ops->call) {                                                \
        return _e->ops->call(_e, ##__VA_ARGS__);                        \
    }                                                                   \
    if (_e->ops->passthru) {                                            \
        int _real = _e->ops->passthru(_e, SYNTH_OP_IO);                 \
        if (_real >= 0) {                                               \
            return sym_next(call, _real, ##__VA_ARGS__);                \
        }                                                               \
    }                                                                   \
    errno = _saved;                                                     \
} while (0)

// Like SYNTH_DISPATCH, but does not fall through to the kernel if the
// synthetic fd lacks a handler or passthru. Instead, it returns -1 with
// EOPNOTSUPP. Use this for operations that natively corrupt O_PATH fds.
#define SYNTH_DISPATCH_EXCL(call, fd, ...) do {                         \
    SYNTH_DISPATCH(call, fd, ##__VA_ARGS__);                            \
    SYNTH_AUTOPIN_FD(_e, fd);                                           \
    if (_e != NULL) {                                                   \
        return errno = EOPNOTSUPP, -1;                                  \
    }                                                                   \
} while (0)

// SYNTH_SHIM_E: shim body that dispatches when the kernel returns the
// given canary errno (the value O_PATH yields for the unsupported op).
// Most ops use EBADF -- see SYNTH_SHIM below -- but readv/writev/p*v
// give EPERM, ftruncate gives EINVAL, fallocate gives ENODEV.
#define SYNTH_SHIM_E(call, canary, ...) do {                            \
    __auto_type _r = sym_next(call, __VA_ARGS__);                       \
    if (_r != -1 || errno != (canary) || wslcompat_passthru(#call))     \
        return _r;                                                      \
    SYNTH_DISPATCH(call, __VA_ARGS__);                                  \
    return _r;                                                          \
} while (0)

// SYNTH_SHIM: convenience wrapper for the common EBADF-canary case.
// Use for thin wrappers in read.c et al; the body of ioctl/fcntl is
// bespoke.
//
// Usage:
//   ssize_t read(int fd, void *buf, size_t count) {
//       SYNTH_SHIM(read, fd, buf, count);
//   }
#define SYNTH_SHIM(call, ...) SYNTH_SHIM_E(call, EBADF, __VA_ARGS__)

// Shim hooks for close-like ops (close, dup2/dup3 onto fd). Pair
// every pre_close with a post_close; post_close is NULL-safe.
//
//   struct synth_entry *e = wslcompat_synth_pre_close(fd);
//   int r = sym_next(close_like_syscall, ...);
//   int saved = errno;
//   wslcompat_synth_post_close(e);
//   errno = saved;
//   return r;
struct synth_entry *wslcompat_synth_pre_close(int fd);
void                wslcompat_synth_post_close(struct synth_entry *e);

// Move any internal fd matching targetfd to a fresh slot before the
// caller's close/dup2/dup3 retires it. Covers probe_fd and handler-
// owned fds (tty's real_fd) so close-all loops mid fork+exec don't
// nuke them.
void wslcompat_synth_relocate_internal(int targetfd);

// Clear a slot array (sets every pid to 0). Use at install time.
void wslcompat_override_init(struct synth_override *slots, size_t n);

// Generic CoW fd accessors. Installer reads/writes the base; other
// processes get/claim an override slot. Used by both synth.c (probe_fd)
// and handlers like tty.c (real_fd).
int  wslcompat_view_fd_load(struct synth_entry *e,
                            struct synth_override *slots, size_t n,
                            atomic_int *base);
void wslcompat_view_fd_store(struct synth_entry *e,
                             struct synth_override *slots, size_t n,
                             atomic_int *base, int new_fd);
int  wslcompat_view_fd_relocate(struct synth_entry *e,
                                struct synth_override *slots, size_t n,
                                atomic_int *base, int targetfd);

// Resolve fd through the passthru callback for the given op kind.
// Returns the backing fd, or -1 if fd isn't synthetic or the handler
// doesn't supply a backing for that kind.
int wslcompat_synth_passthru(int fd, enum synth_op_kind kind);

// True while this process has any published or registering synth entries.
// Callers use this only as a conservative fast-path guard.
int wslcompat_synth_active(void);

// Upper bound on the wraps preexec save/restore can carry across a
// single execve attempt; matches synth.c's synth_table capacity.
#define SYNTH_PREEXEC_MAX 64

// Per-attempt save state. Caller allocates on the stack and zero-
// initializes; threaded through save then (on failure) restore.
struct synth_preexec_state {
    struct {
        int user_fd;       // caller-visible fd holding the wrap
        int saved_fd;      // CLOEXEC fd parking the synth OFD across exec
        int orig_cloexec;  // user_fd's FD_CLOEXEC bit before save
    } slots[SYNTH_PREEXEC_MAX];
    size_t count;
};

// Save: for every wrap reachable from /proc/self/fd, park the synth
// O_PATH at a high fd and dup the handler's SYNTH_OP_IO fd over the
// wrap's user-visible position, preserving FD_CLOEXEC. Call before
// sym_next(execve).
//
// We do not skip CLOEXEC wraps: posix_spawn_file_actions_adddup2 and
// similar can launder a CLOEXEC source into a non-CLOEXEC inheritor,
// and that inheritor must reference real_fd, not the synth O_PATH.
//
// Not thread-safe: assumes no other thread is racing close/dup/open on
// caller-visible fds. Apps that exec from a multithreaded process are
// already in UB territory per POSIX.
void wslcompat_synth_preexec_save(struct synth_preexec_state *st);

// Restore: undo a save by dup3-ing each parked synth O_PATH back over
// its user_fd and dropping the parking fd. Call from execve's failure
// path so the current process keeps its wraps after a failed attempt.
void wslcompat_synth_preexec_restore(struct synth_preexec_state *st);

#endif
