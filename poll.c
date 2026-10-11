#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#include "logging.h"
#include "shim.h"
#include "synth.h"
#include "utils.h"

SHIM_INIT(poll, ppoll, select, pselect);

// We can intercept synthetic file descriptors with minimal overhead for these APIs.
//
// The trick is that we give the application an O_PATH fd, which returns
// unusual error conditions when passed to system calls. We pass the operation
// straight through to the kernel, if there was a special error (select returns
// EPERM for example), we check if it was one of our synthetic fds that we need
// to handle.
//
// In the common case, we need to do nothing. If there is a synthetic fd, we need
// to find it and swap in a real file descriptor.

// If fd is synthetic, request it's owner give us a pollable fd.
static int translate_synth_fd(int fd)
{
    int real = wslcompat_synth_passthru(fd, SYNTH_OP_POLL);

    if (real < 0 || real == fd)
        return -1;

    return real;
}

#define MAX_SWAPS 64

struct poll_swap {
    nfds_t idx;
    int synth_fd;
};

// poll and ppoll dispatch through one function, the variant tells kernel_call
// which symbol to forward to.
enum poll_variant { POLL_VARIANT, PPOLL_VARIANT };

struct poll_ctx {
    enum poll_variant      kind;
    int                    timeout;
    const struct timespec *tmo;
    const sigset_t        *sigmask;
};

// Check each fd to see if we need to swap in a synthetic fds. We need to
// record each swap we perform so we can restore everything on the way out
// (i.e. the kernel returns).
static int poll_swap_in(struct pollfd *fds, nfds_t nfds, struct poll_swap *swaps)
{
    int n = 0;

    for (nfds_t i = 0; i < nfds; i++) {
        int real;

        if ((real = translate_synth_fd(fds[i].fd)) < 0)
            continue;

        if (n >= MAX_SWAPS) {
            wslerror("poll: too many synth fds, leaving fd %d untranslated", fds[i].fd);
            continue;
        }

        swaps[n].idx      = i;
        swaps[n].synth_fd = fds[i].fd;
        fds[i].fd         = real;
        n++;
    }
    return n;
}

// Now restore each of the swaps we performed on syscall entry.
static void poll_swap_out(struct pollfd *fds, const struct poll_swap *swaps, int n_swaps)
{
    for (int j = 0; j < n_swaps; j++)
        fds[swaps[j].idx].fd = swaps[j].synth_fd;
}

// Any non-zero revents on a synth O_PATH fd is bogus -- the kernel
// can't poll the backing meaningfully -- and means we must retry
// against the real fd.
static int poll_canary_fired(struct pollfd *fds, nfds_t nfds)
{
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].revents == 0)
            continue;
        if (translate_synth_fd(fds[i].fd) >= 0) {
            wsltrace("poll: fd=%d revents=0x%x fired canary", fds[i].fd, fds[i].revents);
            return 1;
        }
    }
    return 0;
}

static int kernel_call(const struct poll_ctx *c, struct pollfd *fds, nfds_t n)
{
    if (c->kind == POLL_VARIANT)
        return sym_next(poll, fds, n, c->timeout);
    return sym_next(ppoll, fds, n, c->tmo, c->sigmask);
}

static int kernel_probe(const struct poll_ctx *c, struct pollfd *fds, nfds_t n)
{
    static const struct timespec zero;
    struct poll_ctx probe = *c;

    if (probe.kind == POLL_VARIANT)
        probe.timeout = 0;
    else
        probe.tmo = &zero;
    return kernel_call(&probe, fds, n);
}

// In most cases, we wait for the kernel to signal that there was an O_PATH fd
// in the set. That is our canary to indicate there were some synthetic file
// descriptors.
// If there wasn't we don't have to do anything, if there was we hit
// this slow path where we retry after fixing up the fd set.
static int canary_retry(struct pollfd *user_fds, nfds_t nfds, int fallback,
                        const struct poll_ctx *ctx)
{
    struct poll_swap swaps[MAX_SWAPS];
    int n_swaps;
    int r;

    if ((n_swaps = poll_swap_in(user_fds, nfds, swaps)) == 0) {
        wslwarn("poll: canary fired but no swappable fds, ignored");
        return fallback;
    }

    r = kernel_call(ctx, user_fds, nfds);
    preserve_errno();
    poll_swap_out(user_fds, swaps, n_swaps);
    return r;
}

// So there is a case we can't catch - if an O_PATH fd had none
// of the standard events, the kernel does not indicate to us
// that one of the fd's were invalid.
// Unfortunately, we need to check for that, which we do here.
static int needs_proactive_swap(struct pollfd *fds, nfds_t nfds)
{
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd < 0)
            continue;
        if (fds[i].events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM))
            continue;
        return 1;
    }
    return 0;
}

static int poll_dispatch(struct pollfd *user_fds, nfds_t nfds,
                         const struct poll_ctx *ctx)
{
    int result;

    if (wslcompat_passthru("poll") || !wslcompat_synth_active())
        return kernel_call(ctx, user_fds, nfds);

    if ((result = kernel_probe(ctx, user_fds, nfds)) < 0)
        return result;

    if (needs_proactive_swap(user_fds, nfds)) {
        struct poll_swap swaps[MAX_SWAPS];
        int n_swaps;

        n_swaps = poll_swap_in(user_fds, nfds, swaps);
        wsltrace("poll: proactive swap, n_swaps=%d", n_swaps);
        result = kernel_call(ctx, user_fds, nfds);
        if (n_swaps > 0) {
            preserve_errno();
            poll_swap_out(user_fds, swaps, n_swaps);
        }
        return result;
    }

    if (result == 0)
        result = kernel_call(ctx, user_fds, nfds);

    if (result <= 0 || !poll_canary_fired(user_fds, nfds))
        return result;

    wsldbg("poll: canary fired, retrying");
    return canary_retry(user_fds, nfds, result, ctx);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    struct poll_ctx ctx = { .kind = POLL_VARIANT, .timeout = timeout };
    return poll_dispatch(fds, nfds, &ctx);
}

// FORTIFY_SOURCE redirects poll/ppoll, so we intercept that here.
int __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout, size_t fdslen)
{
    wsltrace("fds=%p nfds=%lu timeout=%d fdslen=%zu", fds, nfds, timeout, fdslen);

    if (nfds > fdslen / sizeof(*fds))
        wslcompat_chk_fail();

    return poll(fds, nfds, timeout);
}

int __ppoll_chk(struct pollfd *fds, nfds_t nfds,
                const struct timespec *tmo,
                const sigset_t *sigmask,
                size_t fdslen)
{
    wsltrace("fds=%p nfds=%lu fdslen=%zu", fds, nfds, fdslen);
    if (nfds > fdslen / sizeof(*fds))
        wslcompat_chk_fail();
    return ppoll(fds, nfds, tmo, sigmask);
}

int ppoll(struct pollfd *fds, nfds_t nfds,
          const struct timespec *tmo, const sigset_t *sigmask)
{
    struct poll_ctx ctx = {
        .kind = PPOLL_VARIANT, .tmo = tmo, .sigmask = sigmask
    };
    return poll_dispatch(fds, nfds, &ctx);
}

// Lifecycle of one call:
//
//   1. CANARY: If the kernel returns -1/EPERM, that indicates there
//      was an O_PATH fd, which lets us know there was a synthetic fd.
//      If we don't see EPERM, we do nothing - zero overhead for the
//      common case!
//   2. SWAP: If we do see EPERM, we walk caller's three sets; for each watched
//      synth fd, record (synth_fd, real_fd, s_in[], r_in[]) then mutate the
//      sets in place to substitute real_fd for synth_fd.
//   3. RETRY: Now we can retry call kernel with translated sets and new_nfds bumped
//      to cover any real_fd >= caller's nfds.
//   4. UNWIND: for each direction the caller watched synth_fd in,
//      mirror real_fd's output onto synth_fd, then clear real_fd
//      from the output unless r_in says the caller watched it too.
//      This requires two passes so synth fds sharing the same real backing all
//      see the event (we must not clear real_fd until every dependent
//      synth has been mirrored).
//   5. RECOUNT: Calculate rv from the final bit count. The kernel's
//      rv counted its (real_fd) bits; we've added synth bits and
//      possibly cleared real ones, so its count is stale.

// Per-swap state.
struct sel_swap {
    int synth_fd;
    int real_fd;
    unsigned char s_in[3];
    unsigned char r_in[3];
};

// Did the caller watch `fd` in `set`?
static int sel_watched(const fd_set *set, int fd)
{
    return set && FD_ISSET(fd, set);
}

// Record each swap, substitute real_fd into the sets, and bump *new_nfds_p to
// cover any real_fd >= nfds.
static int sel_swap_in(int nfds, fd_set *sets[3],
                       struct sel_swap *swaps, int *new_nfds_p)
{
    int n = 0;
    int new_nfds = nfds;

    // Cap at FD_SETSIZE: the stock glibc fd_set is that wide; bits
    // past it aren't ours to probe even if nfds says otherwise.
    if (nfds > FD_SETSIZE) {
        wsldbg("select: nfds=%d exceeds FD_SETSIZE=%d, scanning truncated", nfds, FD_SETSIZE);
    }

    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        int real;

        if (!sel_watched(sets[0], fd) &&
            !sel_watched(sets[1], fd) &&
            !sel_watched(sets[2], fd))
            continue;

        if ((real = translate_synth_fd(fd)) < 0)
            continue;

        if (real >= FD_SETSIZE) {
            wslerror("select: real fd %d for synth %d exceeds FD_SETSIZE", real, fd);
            continue;
        }

        if (n >= MAX_SWAPS) {
            wslerror("select: too many synth fds, leaving fd %d untranslated", fd);
            continue;
        }

        swaps[n].synth_fd = fd;
        swaps[n].real_fd  = real;

        for (int d = 0; d < 3; d++) {
            swaps[n].s_in[d] = sel_watched(sets[d], fd);
            swaps[n].r_in[d] = sel_watched(sets[d], real);
            if (swaps[n].s_in[d]) {
                FD_CLR(fd, sets[d]);
                FD_SET(real, sets[d]);
            }
        }

        wsltrace("select: fd=%d -> real=%d", fd, real);
        n++;

        if (real + 1 > new_nfds)
            new_nfds = real + 1;
    }

    *new_nfds_p = new_nfds;
    return n;
}

// We unwind our modidications on exit in two passes below.
static void sel_unwind(fd_set *sets[3], const struct sel_swap *swaps,
                       int n_swaps)
{
    // Pass 1: mirror real readiness onto every synth that maps to it.
    for (int i = 0; i < n_swaps; i++) {
        for (int d = 0; d < 3; d++) {
            if (!swaps[i].s_in[d])
                continue;
            if (FD_ISSET(swaps[i].real_fd, sets[d]))
                FD_SET(swaps[i].synth_fd, sets[d]);
        }
    }
    // Pass 2: clear the real-fd inserts we added, unless the caller
    // was watching that real_fd in their own right.
    for (int i = 0; i < n_swaps; i++) {
        for (int d = 0; d < 3; d++) {
            if (!swaps[i].s_in[d])
                continue;
            if (!swaps[i].r_in[d])
                FD_CLR(swaps[i].real_fd, sets[d]);
        }
    }
}

// Now recount bits, the rv = total bits set in the returned sets.
// The kernel's original count might be stale after unwind.
static int sel_recount(int nfds, fd_set *sets[3])
{
    int n = 0;
    // Same FD_SETSIZE clamp as sel_swap_in -- bits past the stock fd_set
    // storage aren't ours to read.
    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        for (int d = 0; d < 3; d++) {
            if (sel_watched(sets[d], fd))
                n++;
        }
    }
    return n;
}

enum sel_variant { SELECT_VARIANT, PSELECT_VARIANT };

struct sel_ctx {
    enum sel_variant       kind;
    struct timeval        *tv;       // select (writable, NULL => infinite)
    const struct timespec *tmo;      // pselect (read-only)
    const sigset_t        *sigmask;  // pselect
};

// Forward the user's tv/tmo directly. Linux only updates tv on
// success or timeout, not on -1/EPERM, so the canary retry sees the
// original; and on success the kernel writes back unslept time for us.
static int sel_kernel_call(const struct sel_ctx *c, int nfds,
                           fd_set *r, fd_set *w, fd_set *e)
{
    if (c->kind == SELECT_VARIANT)
        return sym_next(select, nfds, r, w, e, c->tv);
    return sym_next(pselect, nfds, r, w, e, c->tmo, c->sigmask);
}

// Pure kernel passthrough. WSL1 select returns -1/EPERM if any input
// fd is an O_PATH wrap -- that's our canary. On EPERM we swap synth
// fds for their real backings.
static int sel_dispatch(int nfds, fd_set *r, fd_set *w, fd_set *e,
                        const struct sel_ctx *ctx)
{
    struct sel_swap swaps[MAX_SWAPS];
    int rv = sel_kernel_call(ctx, nfds, r, w, e);
    int eff_nfds = nfds;
    int n_swaps;
    fd_set *sets[3] = { r, w, e };

    if (rv >= 0 || errno != EPERM)
        return rv;

    if (wslcompat_passthru("select")) {
        return rv;
    } else {
        preserve_errno();
        if ((n_swaps = sel_swap_in(nfds, sets, swaps, &eff_nfds)) == 0)
            return rv;
    }

    rv = sel_kernel_call(ctx, eff_nfds, r, w, e);

    preserve_errno();

    if (rv > 0) {
        // Count the ready bits that the kernel set below FD_SETSIZE
        int k_low = sel_recount(eff_nfds, sets);

        // Any remaining bits must be >= FD_SETSIZE
        int k_high = rv - k_low;

        // Unwind the synthetic fds (only affects bits < FD_SETSIZE)
        sel_unwind(sets, swaps, n_swaps);

        // Recount the low bits after unwinding, and add back the high bits!
        rv = k_high + sel_recount(nfds, sets);
    } else {
        sel_unwind(sets, swaps, n_swaps);
    }
    return rv;
}

int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    struct sel_ctx ctx = { .kind = SELECT_VARIANT, .tv = tv };
    return sel_dispatch(nfds, r, w, e, &ctx);
}

int pselect(int nfds, fd_set *r, fd_set *w, fd_set *e,
            const struct timespec *tmo, const sigset_t *sigmask)
{
    struct sel_ctx ctx = {
        .kind = PSELECT_VARIANT, .tmo = tmo, .sigmask = sigmask
    };
    return sel_dispatch(nfds, r, w, e, &ctx);
}
