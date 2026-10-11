# Wrap-state lifecycle across vfork / CLONE_VM

This is the "why is the synth code so complicated" doc. Read this
before touching `synth.c` or `tty.c`'s override machinery.

## The problem

Running `gdb /usr/bin/top` with libwslcompat loaded broke `top`:

- Without libwslcompat: works.
- With libwslcompat, running top standalone: works.
- With libwslcompat, top under gdb: top hits SIGTTOU on its first
  `tcsetattr` and gdb gets confused.

gdb uses `vfork()` to spawn its inferior. `vfork()` is `CLONE_VM` (parent
and child share the address space) but NOT `CLONE_FILES` (separate fd
tables). The parent is suspended until the child execs or exits.

The bug chain we ended up untangling:

1. gdb wraps its own stdin (TCSETS triggers `tty_install`). Synth fd 0
   replaces gdb's stdin; `real_fd` lives at a high fd (e.g. fd 12).
2. gdb vforks. Child has its own fd table (copy at vfork-time) but
   SHARES memory with parent. Both processes see the same
   `synth_table`, `tty_state`, all of libwslcompat's data.
3. Child does `close-all` (closes fds 3..rlim) to avoid leaking gdb's
   internal fds to the inferior.
4. close-all closes fd 12 in the child's fd table → our `close` shim
   fires → `relocate` machinery walks the synth table → finds
   `tty_state.real_fd == 12` matches the fd being closed.
5. *In the first design*, relocate did F_DUPFD in the child's fd table
   to save the OFD AND mutated shared `tty_state.real_fd = new_fd` so
   subsequent uses found the relocated fd.
6. Child execs top. Parent resumes.
7. **Parent's `tty_state.real_fd` is now the value the child wrote** —
   an fd that exists in the (now-defunct) child's fd table, not in
   parent's. Every subsequent ioctl/read/fcntl gdb does on its wrapped
   stdin uses the wrong fd, fails, and gdb's job-control code can't
   `tcsetpgrp` the inferior to fg. Top runs in a bg pgrp, hits
   SIGTTOU on its first tty output.

The core issue: **mutating shared memory from a vfork-CLONE_VM child
corrupts the parent's view.** The parent had nothing to do with the
close-all, but its state was clobbered anyway.

## What we tried that didn't work

- **execve shims via the normal libc entry points** (execve, execvp,
  execl-family, system, popen, fexecve, posix_spawn). gdb bypasses our
  libc-symbol shims by calling glibc-internal `__execve` (direct-bound,
  not interposed) — and for `tcsetpgrp` it does the same with `__ioctl`.
  We added shims for every libc symbol we could think of; it got us
  past the EBADF-on-inherited-O_PATH problem but not the corruption.

- **`fstat` shim that reports the backing tty's stat** so apps that
  gate behavior on `S_ISCHR` (gdb does) work correctly. Helpful in
  general — kept. Did not fix this bug because the issue wasn't the
  detection, it was the corruption after detection.

- **ttyname-based recovery in `preexec_save`**. Cache the tty's path
  at install, reopen by path in the child if the cached `real_fd` is
  invalid. Solved the symptom (child gets a working tty fd before
  exec) but didn't address the parent-corruption root cause. And it
  loses OFD-level state (file status flags); user pushed back.

- **Pid-check on mutation** (single-slot per-entry override). Don't
  mutate shared state from a non-installer process. Worked for
  sequential vforks but a single override slot per entry would get
  clobbered in nested-vfork scenarios.

## What we ended up with: multi-slot CoW per entry

The mental model:

- Every `synth_entry` (and handler-specific state like `tty_state`)
  has a **base** fd value, owned by the *installer* process.
- It also has **N override slots**, each `(pid, starttime, fd)`. A
  process that isn't the installer can claim one of these and put its
  own fd value in it.
- Readers compose: "if I'm the installer, use base. Otherwise, find my
  slot in the array; if found, use it; if not, fall back to base."
- Writers similarly: installer writes base; non-installers claim a
  slot and write into it.

In CLONE_VM land, **writes by the child don't touch the parent's view**
because the child's writes go to a different storage location (its
override slot). The parent's reads ignore override slots that don't
match the parent's `(pid, starttime)` identity.

The handler-side code stays clean — each accessor is a one-line
forwarder to the generic `wslcompat_view_fd_load/store` in synth.c.

### Why pid + starttime, not pid alone

Pids can be reused. After a vfork-child exits, its override slot
lingers (until GC). If Linux recycles that pid to some other process
in our shared address space, lookups would falsely match the dead
child's stale slot. `starttime` (field 22 of `/proc/PID/stat`,
jiffies-since-boot the process started) uniquely identifies a process
*instance* — same pid + same starttime can never be two different
processes.

### GC

`wslcompat_override_acquire` runs `wslcompat_override_gc` first. GC
walks the slot array and, for each non-free slot, reads
`/proc/<pid>/stat` to check if the starttime still matches what we
stored. If not (process exited, or pid was recycled to a new process
with different starttime), the slot is cleared.

This means dead slots get reclaimed lazily, only when someone tries
to claim a new slot in the same array.

## Code map

### `synth.h`

```c
struct synth_override { _Atomic pid_t pid; unsigned long long starttime; int fd; };
#define SYNTH_OVERRIDE_SLOTS 4

struct synth_entry {
    ... base fields (probe_fd, etc) ...
    pid_t              install_pid;
    unsigned long long install_starttime;
    struct synth_override probe_overrides[SYNTH_OVERRIDE_SLOTS];
};
```

Exposed helpers:
- `wslcompat_self_starttime()` — cached per-pid; refreshes on vfork.
- `wslcompat_override_init/get/acquire/gc` — slot lifecycle.
- `wslcompat_view_fd_load/store` — the generic accessor pattern that
  every per-process-CoW fd should go through. Uses internal `_pid` variants
  to consolidate `getpid()` and identity lookups on the hot path.

### `synth.c`

- `read_proc_starttime` parses `/proc/PID/stat` field 22. Uses `sym_next(read)`
  to bypass the library's own read shim.
- `wslcompat_self_starttime` caches with acquire/release on `cache_pid`.
- `wslcompat_override_acquire_pid` uses a fixed publication order: it writes
  `starttime` while the slot is still "free" (`pid == 0`), then performs a
  `memory_order_release` store on the `pid` to publish the entire slot.
- `synth_probe_fd_load/store` are one-line forwarders to the generic
  helpers, applied to `probe_fd` and `probe_overrides`.

### `tty.c`

- `struct tty_state` has `real_fd` and `real_overrides[N]`.
- `tty_real_fd_load/store` are one-line forwarders to the generic
  helpers, applied to `real_fd` and `real_overrides`.
- Every tty op (`tty_read`, `tty_ioctl`, `tty_fcntl`, `tty_passthru`,
  `tty_relocate`) calls `tty_real_fd_load(s, e)` to get the
  right-per-process fd value.
- `tty_install` claims the installer slot (sets `install_pid` etc on
  the synth_entry) and initializes `real_overrides`.

### Tests

In `tests/exec_wrappers_test.c`:
- `test_close_all_then_exec` — fork-style close-all (the gdb pattern,
  but without vfork).
- `test_vfork_close_all_preserves_parent` — vfork close-all, verify
  parent's wrap still works (the original bug).
- `test_vfork_close_all_child_sees_tty` — vfork close-all, verify
  the child's exec'd image sees a real tty (proves the override is
  being used).
- `test_vfork_repeated` — 8 sequential vforks.
- `test_vfork_stress_gc` — 50 sequential vforks, would exhaust the
  4 override slots if GC didn't reclaim.

In `tests/vfork_stress_test.c`:
- **Concurrent vfork stress** — Spawns 8 threads that concurrently `vfork`
  and relocate the same shared TTY wrap, while a probe thread verifies
  the parent's view remains uncorrupted. Hammers the identity caching
  and slot publication logic.

## What we don't handle

1. **Inline-assembly syscalls.** A process that issues exec, ioctl,
   close, etc. via raw `syscall` instruction bypasses our shims
   entirely. We've covered everything that goes through libc symbols,
   including `syscall(SYS_execve)` via our syscall shim. But true
   inline-asm syscalls are invisible to us.

2. **Override slot exhaustion under extreme pressure.** Each entry has
   4 slots. If more than 4 *currently-live* processes need to override
   the same entry simultaneously, the 5th's `override_acquire` returns
   NULL and the operation is silently dropped (logged as warning). For
   sequential vforks (the common case) this never happens because GC
   reclaims dead slots. For truly concurrent CLONE_VM children (would
   require, e.g., a multithreaded program where multiple threads vfork
   simultaneously, which is POSIX-undefined) we'd lose overrides.

3. **GC false negatives on pid reuse within the same starttime tick.**
   Theoretically: process A exits, pid reused to process B that happens
   to start in the same jiffy. starttime would match. We'd treat B's
   slot as A's. Vanishingly unlikely (jiffies are typically 100Hz or
   1000Hz; getting two processes in the same tick AND reusing pid that
   fast is implausible). Worth knowing about but not worth designing
   around.

4. **The parent doing its own close-all in normal operation.** We used
   to handle this with relocate-on-close that mutated state in place.
   The new design's "installer" mutation path still works for the
   parent's own close-all, so this case still works *if* the installer
   is the one doing the close. What we lost is "parent forks, then
   parent does close-all in normal operation" — but fork doesn't share
   memory, so each side is fine on its own copy.

5. **`/proc` not mounted.** Without `/proc`, `read_proc_starttime`
   returns 0, every starttime comparison passes/fails arbitrarily, GC
   becomes ineffective. WSL1 always has /proc; we don't worry about
   this. Recovery via `preexec_save_one` still walks `/proc/self/fd`
   so this would already be broken.

6. **Nested vfork (A vforks B while A is in vfork-child state).**
   POSIX undefined behavior. With 4 slots per entry, the design can
   in principle accommodate this (each of P, A, B claims its own
   slot). Untested because we can't write a sane test for UB.

7. **Cross-binary exec where the inferior loads a DIFFERENT version
   of libwslcompat.** Different `struct synth_entry` layouts. The
   inherited shared memory is inherited with the new layout. Could
   corrupt. Don't ship ABI changes without thinking about this.

## Things to remember tomorrow

- The CoW machinery lives in `synth.c`. Handlers (just tty today)
  thread their own fd state through `wslcompat_view_fd_load/store`.
- The hot path (tty_read) performs **exactly one** `getpid()` and one
  cached `self_starttime()` check. This identity is passed down through
  `_pid` variants of the lookup functions to minimize syscall overhead.
- `starttime` IS cached, invalidated on `getpid()` change. Vfork-child
  refreshes automatically on first use.
- The publication order in `acquire` is critical: write payload first,
  then release-store the PID to lock the slot. GC uses acquire-load on
  the PID to ensure it sees the payload.
- Internal liveness checks for `/proc/PID/stat` use `sym_next(read)` to
  avoid the library's own read shim overhead.
