# Why ttys complicate the wait shims

A refresher on the tty handler and the ripple it creates through
poll/select/epoll. The synth machinery itself is elegant; tty's
specific shape is where the complexity comes from.

## The wrap and why the kernel rejects ops on it

Every synth fd handed back to the user is an `O_PATH` ref to an
unlinked `O_TMPFILE`. The kernel rejects most ops on `O_PATH` —
`read`/`write` get `EBADF`, `poll` returns immediate `POLLIN`,
`select` returns `EPERM` — and our shims catch those canary errors
and dispatch to handler callbacks. That's the "ask the kernel first,
fix up only on canary" pattern that makes non-synth fds zero-cost.

For purely synthetic files (the `/proc/sys/vm/mmap_min_addr` polyfill,
`/proc/PID/mem`, etc.) this is the whole story. Their `read`/`write`
callbacks synthesise content; there's no real kernel fd underneath.

## Why tty is different: cooking needs a real fd

WSL1's tty driver doesn't honour non-canonical mode (VMIN/VTIME), so
when an app sets `ICANON` off and asks for "block until 1 byte," we
have to fake it ourselves: poll the real slave fd, then read with the
VMIN/VTIME contract. `tcsetattr` also needs to actually run on the
real kernel fd. None of that works on an `O_PATH`.

So at `tty_install` time we dup the user's original slave fd into our
own `real_fd`, then dup3 the wrap over the user's fd. The user now
holds an `O_PATH` (the wrap); we hold the real RDWR slave OFD via
`real_fd`. Our `tty_read`/`tty_ioctl`/`tty_fcntl` operate on
`real_fd`. Cooking works.

## The hidden anchor

The slave is a kernel OFD. The kernel fires `POLLHUP` on the master
when the slave's OFD refcount drops to zero. Our `real_fd` is one of
those refs. As long as we hold it, the master can't `POLLHUP`.

In a single process this is benign: when the user closes the wrap,
our close shim fires `tty_release`, which closes `real_fd`. Anchor
gone, kernel POLLHUPs the master if no one else has the slave.

Cross-process is where it breaks. Parent opens the pty, wraps the
slave, forks. Child has the wrap AND has its own inherited copy of
`real_fd`. Child closes its wrap; child's `tty_release` would fire,
*except* the close shim's flock liveness probe says "parent still has
the wrap" and refuses to release. Even if it did release: parent
still has its own `real_fd`. Master never POLLHUPs.

For the parent to POLLHUP its master while staying blocked in
`poll(master)`, *something* has to release `real_fd` from inside the
parent's process while it's not making any other calls.

## How `real_fd` gets released

The `close` shim's `post_close` decides when to drop the producer
ref (which fires `tty_release` → closes `real_fd`). Two-stage check:

1. **Fast path: flock `LOCK_EX` on `probe_fd`.** Succeeds iff no
   `LOCK_SH` holder exists *anywhere* (any process). One syscall.
   When this succeeds we release immediately — common case for a
   single-process wrap close.

2. **Contested fallback: walk `/proc/self/fd`.** flock can't tell
   same-process dup from cross-process fork inheritance (both share
   the OFD via the kernel fd table). When flock fails, walk our own
   `/proc/self/fd` to find any other fd in *this* process referencing
   the wrap. If none, release. If yes (genuine dup), defer.

Plus an `install_pid` gate: only the installer process is allowed to
release. Non-installer descendants (fork or vfork children) defer
unconditionally — their state cleans up at process exit via the
kernel. This is critical for vfork (`CLONE_VM`): the child shares
parent memory, so a child-side release would mutate parent's
`synth_table` entry and corrupt parent's `real_fd`.

The result: each process manages its own `real_fd` lifecycle locally.
Cross-process master `POLLHUP` works naturally because the kernel
handles cross-process OFD refcounting — there's no hidden anchor
holding it back globally.

## What this buys

- Wait shims (`poll`/`select`/`epoll`) are pure kernel passthrough.
  Select adds a canary-retry path on `EPERM` (translate wraps to
  real backings, retry once, recount). Epoll dispatches synth fds
  via passthru at `epoll_ctl` time. No inotify splice anywhere.
- Zero overhead for any non-synth fd, in any process.
- No `SEGV` risk from copying the user's pollfd buffer.
- No `EINTR` loss in the wait shims (no retry loop to lose signals
  between iterations).

## Where to look if revisiting

The release decision in `post_close` is the load-bearing piece. It's
trying to answer "should THIS process drop its anchor?" — which is
a per-process question. The flock fast path + walk fallback + install
gate together give the right answer cheaply for every case we test.
If a future change needs to add per-handler customisation, the
existing `close` callback in `synth_ops` is the hook.

See `PROBE_REFIRE.md` for the re-probe alternatives (transient
`real_fd`, signal-driven drain, inotify splice, background thread)
and why the wait shims stayed passthrough.
