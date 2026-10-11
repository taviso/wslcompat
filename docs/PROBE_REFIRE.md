# Re-firing the synth-fd liveness probe

> **Superseded.** The `/proc/self/fd` walk fallback in `synth_should_release`
> (see `TTY_DESIGN.md`) fixed the POLLHUP hang without any of the machinery
> below. Kept as the record of the inotify analysis.

The `flock(LOCK_SH)` probe in `synth.c` correctly detects when the
user's last reference to a synth fd is gone, but we only run it from
`post_close`. Any release that happens AFTER that point (child exits,
child closes inherited fd, SCM_RIGHTS hand-off ends) goes unnoticed.
For tty wraps that means `real_fd` lingers and a pty master never
sees POLLHUP -- the emerge hang.

Captured by `pty_master_pollhup_test` (wait*-fixable) and
`pty_childclose_alive_test` (not wait*-fixable). The kernel-side
probe model is sound -- see `probe_semantics_test`.

## Options considered

| Approach | Coverage | Cost / Complexity |
|---|---|---|
| **wait\* shim** | Catches child-exit cases (the common shape). Misses child-closes-stays-alive. | Trivial. One probe walk on `wait*` return. |
| **Opportunistic re-probe on canary fire** | Catches anything where the user touches a synth after the release. Misses "user blocked in `epoll_wait` already". | Cheap. One walk per shim entry; gated by canary errno so zero overhead when user has no synths. |
| **Timeout loop in poll/select/epoll_wait** | Catches the blocked-in-wait case too: chop indefinite waits into N-ms iterations, re-probe between. Gate on "any alive synth" to skip the loop entirely for users with no wraps. | Bounded wake-up jitter (N ms). Needs new `epoll_wait` family shims. `select`'s timeval-update wart must be preserved. Tunable interval. |
| **inotify+signal (`F_SETSIG` on inotify_fd)** | Truly async on stock Linux: kernel signals us on every close event on the watched inode, handler re-probes. | DOA on WSL1: F_SETOWN and F_SETSIG both return EINVAL; O_ASYNC accepts the flag but with no owner nothing is delivered. |
| **inotify_fd merged into user's wait set** | True async wakeup we own. Splice our inotify_fd into the fd_set / pollfd[] / epoll set the user passes to select/poll/epoll_wait; when our fd fires we drain, re-probe, then loop or return user-visible events. | Three new wait shims plus the array-extension fiddliness for poll. Verified viable on WSL1 (see `inotify_path_close_test`). Catches the blocked-in-wait gap. |
| **pthread per synth, blocking `flock(LOCK_EX)`** | Truly async: kernel grants `LOCK_EX` the instant the synth OFD dies. Covers everything. | Threads inside a lockless `LD_PRELOAD` shim -- fork semantics, glibc init ordering, signal interaction, TLS. |

## WSL1 inotify capability surface (from `inotify_path_close_test`)

- `inotify_init1`, `inotify_add_watch`: work fully.
- Watches survive `unlink` of the watched name (we work on unlinked
  tmpfiles).
- `IN_CLOSE_NOWRITE` fires for `O_PATH` fd closes -- our synth wraps.
- `inotify_fd` is pollable via `poll`/`select`/`epoll_wait`.
- Cross-process events propagate for fresh opens (child does its own
  `open`+`close` on the watched inode -> parent sees both events).
- **WSL1 quirk**: when a child closes an INHERITED fd but the OFD
  itself survives (parent still has a ref), inotify does NOT fire.
  Stock Linux fires on every per-fd close; WSL1 only fires on OFD
  creation and OFD destruction.
- The destruction event DOES fire cross-process: when the last
  reference to the OFD (in any process) is dropped, parent's
  `inotify_fd` becomes readable. This is exactly the "synth fd
  finally died" signal we want.

So the inotify-in-fdset approach is viable on WSL1. The semantic gap
(no signal on intermediate per-fd closes) is harmless for us -- we
only care about the last close, which is the OFD destruction event,
which does fire.

## fcntl signal-delivery probe (`F_SETOWN`/`F_SETSIG`/`O_ASYNC`)

One-shot probe in `/tmp`:

```
inotify_init1:           OK
F_SETOWN(getpid):        -1 EINVAL
F_SETSIG(SIGRTMIN+1):    -1 EINVAL
F_SETFL(+O_ASYNC):       OK (flag set, no delivery target)
inotify_add_watch:       OK
SIGIO / SIGRTMIN+1:      never fired
inotify read:            events delivered correctly
```

So async signal delivery via fcntl is not a tool we have on WSL1.
Inotify itself works, but we have to read its fd ourselves (no kernel
poke).

## Sketch for the timeout-loop variant (kept for reference)

```c
int poll(struct pollfd *fds, nfds_t n, int timeout) {
    if (atomic_load(&alive_synth_count) == 0)
        return sym_next(poll, fds, n, timeout);
    int remaining = timeout;  // -1 stays -1
    for (;;) {
        int iter = remaining < 0 ? REPROBE_INTERVAL_MS
                                 : MIN(REPROBE_INTERVAL_MS, remaining);
        long t0 = now_ms();
        int r = sym_next(poll, fds, n, iter);
        if (r != 0) return r;          // real events / EINTR -- bail
        reprobe_synth_table();         // may make POLLHUP fire on caller fds
        if (remaining >= 0) {
            remaining -= (now_ms() - t0);
            if (remaining <= 0) return 0;
        }
    }
}
```

`select` needs `*tv = ms_to_timeval(remaining)` before each return to
honour the "kernel writes back remaining time" contract. `epoll_wait`
needs three new shims (`epoll_wait`, `epoll_pwait`, `epoll_pwait2`).

## Current state

**epoll + poll + select families all done.** XPASS coverage:
`pty_master_pollhup_test`, `pty_childclose_alive_test`,
`pty_childclose_poll_test`, `pty_childclose_select_test`,
`asyncio_test`, plus fork-ordering tripwires
(`epoll_fork_order_test`, `epoll_after_fork_test`,
`poll_fork_order_test`, `select_fork_order_test`) and extremes
(`poll_extremes_test`, `select_extremes_test`).

Common infrastructure in `synth.c`:
- Global `inotify_fd`, lazy-opened via `synth_inotify_fd()` with
  pid-mismatch detection (handles fork without `pthread_atfork`;
  vfork is covered by the same lazy check).
- `wslcompat_synth_open` calls `inotify_add_watch` on
  `/proc/self/fd/<path_fd>` with `IN_CLOSE_NOWRITE | IN_CLOSE_WRITE`
  after register; watch descriptor stashed in `synth_entry.inotify_wd`.
- `wslcompat_synth_drain_and_reprobe()` drains pending inotify
  events and runs the `LOCK_EX|LOCK_NB` probe across every live slot.

### epoll (`epoll.c`)

- `epoll_create`/`epoll_create1`: register our inotify_fd in the new
  epoll set with a pointer-identity sentinel via `register_inotify`.
- `epoll_wait`/`epoll_pwait`: call `register_inotify(epfd)` at entry
  so a child that inherited an epfd plus a fresh-after-fork
  inotify_fd registers the new fd on the next wait (EEXIST is
  tolerated as the steady-state case). Loop, filter the sentinel,
  drain+reprobe when it fires, return when user-visible events
  remain or the deadline elapses. Stale parent-process inotify_fd
  registration in an inherited epfd persists -- can't be cleaned up
  since we no longer have a fd referencing it -- and produces
  occasional drain-to-no-op spurious wakes. Acceptable tradeoff vs.
  missing wakes.

### poll / ppoll (`poll.c`)

- alloca a `struct pollfd[nfds + 1]`, copy user fds in, splice
  `inotify_fd` into the extra slot, loop the kernel call on the
  extended array.
- Each iteration: mirror revents back to user fds; if the inotify
  slot fired, drain+reprobe and continue with adjusted remaining
  time; if any user fd shows the synth-O_PATH canary, fall through
  to the legacy swap-in / retry / swap-out path on the user's
  original array.
- Bypass path (`nfds >= MAX_POLL_FDS = 1024`, or `inotify_fd < 0`):
  single-shot kernel call + canary retry, no async wake.
- ppoll converts its `timespec` timeout to ms for the deadline math
  and rebuilds a `timespec` for each kernel call. Sub-ms precision
  is lost, acceptable for the wake purpose.

### select / pselect (`poll.c`)

- Preemptive swap of synth O_PATHs to handler real fds in the user's
  three fd_sets, then splice `inotify_fd` into rfds. If user passed
  NULL rfds, a stack-local fd_set holds the inotify entry. If
  `inotify_fd >= FD_SETSIZE` we skip the splice (async wake lost,
  canary-fire re-probe still active).
- Loop the kernel call with deadline-derived remaining; on inotify
  fire drain+reprobe and FD_CLR the inotify bit; on real user events
  break out, unwind the swap, recount, and return.
- For select(), the unslept portion of the original timeout is
  written back into the user's tv on every return path (we pass the
  kernel a local timeval rather than the user's, so we own the
  contract update).
- Fast path (no synth fds and inotify unavailable) is a single
  direct kernel call -- no loop, no save/restore.

## Open items

- **epoll_pwait2** (Linux 5.11+) -- absent on WSL1 today; adding it
  naively to `SHIM_INIT` would break the whole shim if dlsym fails.
  Guard with a conditional dlsym or skip until someone hits it.
- **Stale parent inotify_fd registration in inherited epfds** --
  see above; harmless spurious wakes, no clean removal path.
- **MAX_SWAPS = 64 cap in select's slow path** -- pre-existing;
  callers with >64 synth fds in one select() see EPERM with the
  excess fds left as O_PATH. Same as before the inotify work.

The inotify-in-wait-set trick is more plumbing than ideal and was
never implemented: the `/proc/self/fd` walk fallback in
`synth_should_release` (see `TTY_DESIGN.md`) releases `real_fd` on
the close path, which covers the emerge scenario without any of this.

Threading and fcntl-signal both ruled out by WSL1 (see capability
probes above).
