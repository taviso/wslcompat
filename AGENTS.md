# wslcompat — Agent Guide

## Overview

wslcompat is an `LD_PRELOAD` shared library (glibc-only, Linux x86-64) that polyfills missing or broken syscall behavior on **WSL1**. It intercepts libc functions via `RTLD_NEXT` and either fixes up arguments before forwarding, synthesizes missing kernel objects, or transparently upgrades broken primitives to working ones.

**Key constraints:**
- Must survive `LD_PRELOAD` double-load (two link maps for the same `.so`)
- Must survive `vfork`/`CLONE_VM` without corrupting parent state
- Must work with existing binaries — no recompilation required

## Build & Test

```bash
# Build the library and tools
make

# Build with debug symbols
make debug

# Run the test suite (requires valgrind and patchelf in PATH)
make test

# Install (default prefix: /usr/local)
make install

# Build-only the test targets without running
make -C tests
```

- **Tests are parallel by default** (`make -j$(nproc)` is baked into the tests Makefile)
- **Tests have categories**:
  - `TESTS` — must fail without the shim, pass with it
  - `XFAIL` — expected failures (known-unfixable)
  - `XPASS` — must pass both with and without the shim
  - `XFIXME` — known regressions (pass without, fail with)
- **Valgrind runs** on every `fix_*` (patchelf'd) binary
- **When run as root**, the test suite switches to a privilege-escalation suite (`priv_drop_exec_test`, `priv_close_loop_exec_test`)

## Project Structure

```
wslcompat/
├── GNUmakefile          # Top-level build
├── shim.h               # Core shim infrastructure (RTLD_NEXT, SHIM_INIT, sym_next)
├── synth.h / synth.c    # Synthetic fd framework (the big subsystem)
├── tunables.h / tunables.c  # Per-binary xattr-based configuration
├── logging.h / logging.c    # Debug logging to /dev/tty or a logfile
├── internal.h / internal.c  # Rescue internal fds from close/dup2 conflicts
├── syscall.c            # syscall() shim (redirect via user.wslcompat.syscall)
├── open.c               # open/openat shim (includes proc.c for path dispatch)
├── proc.c               # /proc path handlers (mmap_min_addr, /proc/PID/mem)
├── *.c (other shims)    # One file per polyfilled libc function
├── tty.c                # TTY synth handler (the most complex synth consumer)
├── tools/
│   └── elfclamp.c       # Standalone tool to unify PT_LOAD p_align in ELF64 binaries
├── tests/               # ~100 test programs, C + Python + shell
│   ├── GNUmakefile      # Test orchestration with category-aware rules
│   └── testutil.h       # wslcompat_test_on_wsl(), wslcompat_test_require_or_skip()
├── SYSCALLS.md         # List of syscalls WSL1 provides
├── LOCKS.md            # File locking limitations on WSL1
├── TUNABLES.md         # Full tunable reference
└── docs/
    ├── INTERNAL_CALLS.md   # The 4 call patterns for kernel access (MUST READ)
    ├── TTY_DESIGN.md       # Why ttys need the synth framework
    ├── VFORK_COW.md        # CoW override slot design for vfork safety
    ├── PROBE_REFIRE.md     # Re-firing the synth-fd liveness probe (inotify design, not wired in)
    └── NOTES.md            # Support guarantees (what we can/cannot polyfill)
```

## Architecture: Polyfill Patterns

### 1. Simple argument fixup shim

The most common pattern: strip unsupported flags, translate between primitives, then forward.

```c
// mmap.c, getsockopt.c, setsockopt.c, clock.c
// - Check wslcompat_passthru_self() first (tunable bypass)
// - Fix up arguments
// - Call sym_next(...) to forward
```

### 2. canary-dispatch shim (EBADF/EPERM)

For synthetic fds: let the kernel try first, catch the predictable error, then dispatch to the synth handler.

```c
// read.c, write.c, close.c, ioctl.c, poll.c
ssize_t read(int fd, void *buf, size_t count) {
    ssize_t r = sym_next(read, fd, buf, count);
    if (r != -1 || errno != EBADF) return r;
    SYNTH_DISPATCH(read, fd, buf, count);
    return r;
}
```

The `SYNTH_DISPATCH` macro in `synth.h` handles lookup, pinning, and callback dispatch. Zero cost for non-synth fds.

### 3. Exec wrappers

All exec-family functions (`execve`, `execvp`, `execl`, `fexecve`, `posix_spawn`, `system`, `popen`) are shimmed. They save/restore synth state via `wslcompat_synth_preexec_save`/`restore` so that O_PATH synth wraps get replaced with real kernel fds before exec. The `EXEC_REPLACE_SHIM` macro in `execve.c` is the template.

### 4. syscall() shim

`syscall.c` intercepts `syscall()` itself (for programs that use `syscall(SYS_foo)`). By default it is a pure passthrough. The `user.wslcompat.syscall` tunable redirects specific syscalls through their polyfill; a thread-local `redir` guard prevents re-entry cycles on double-load.

## The Synthetic FD Framework (synth.c / synth.h)

The most important subsystem. It creates fake file descriptors that look real to the application.

**How it works:**
1. `wslcompat_synth_open()` creates an `O_TMPFILE` in `/tmp`, unlinks it, and returns an `O_PATH` fd pointing at the deleted tempfile
2. A `synth_entry` is registered in a global table, keyed by the tempfile's inode
3. The entry holds: a `probe_fd` (the tempfile, for liveness detection via `flock`), an op table (`struct synth_ops`), a userptr, and CoW override slots
4. When the kernel rejects an op on the O_PATH fd (EBADF, EPERM), our shims catch the canary error and dispatch to the handler callbacks

**Key structs:**
- `struct synth_ops` — function pointer table for read/write/ioctl/fcntl/close/etc.
- `struct synth_entry` — one per synthesized fd, refcounted
- `struct synth_override` — per-process CoW override slot (pid + starttime + fd)

**Liveness detection:** Each entry has a `probe_fd` (the O_TMPFILE) that supports `flock`. When the last reference to the synth fd is closed (in any process), the OFD dies, the `flock` is released, and a `LOCK_EX|LOCK_NB` probe succeeds — triggering cleanup.

**Release detection:** the close shim's `post_close` decides release via a `flock` fast-path plus a `/proc/self/fd` walk fallback, so a process drops its own `real_fd` even when a fork child still holds the wrap. No inotify — `docs/PROBE_REFIRE.md` explores that alternative, but the walk fallback made it unnecessary.

## The Four Call Patterns (docs/INTERNAL_CALLS.md)

This is **critical** to understand before modifying any shim:

| Pattern | When | Example |
|---------|------|---------|
| `syscall(SYS_foo, ...)` | Internal bookkeeping, constructors, internal fds | `syscall(SYS_fstat, ...)` |
| `sym_next(foo, ...)` | Inside a shim body, forwarding to libc | `sym_next(read, fd, buf, count)` |
| Bare `foo(...)` (libc name) | You want your own polyfill to fire | `open("/tmp", O_TMPFILE, ...)` |
| `SYSCALL_REDIRECT(fn)` in `syscall.c` | Syscalls opted in via `user.wslcompat.syscall` | `execve`, `mmap` |

**Never** use `sym_next` from a constructor or from internal helpers called by constructors — it can create cycles when the library is double-loaded. Use `syscall()` instead.

**Never** add a plain `case SYS_foo:` that does real work in `syscall.c`; `SYSCALL_REDIRECT` is the only sanctioned hook and relies on its `redir` guard to break re-entry.

When calling a libc function bare (not via `sym_next`) to intentionally trigger your own polyfill, **add a comment** naming the polyfill you depend on.

## Tunables System (tunables.h / tunables.c)

Per-binary configuration via extended attributes (`user.wslcompat.*`).

**How it works:**
- At library init (`constructor`), `tunables.c` reads xattrs from `/proc/self/exe` using `flistxattr`/`fgetxattr`
- Up to 32 key-value pairs cached in a sorted array, looked up via `lsearch`
- **Tunables are per-binary**, not per-process — set on the executable file itself
- Commands: `setfattr -n user.wslcompat.NAME -v VALUE /path/to/program`

**Key tunables:**
- `disabled` / `enabled` — control which polyfills are active
- `debug` — log level 0-3 (output to `/dev/tty` or `user.wslcompat.logfile`)
- `redirect` — enable/disable `/proc` path synthesis (default true)
- `ptinterp` — path to ld-linux for ELF fixup re-exec (default `/lib64/ld-linux-x86-64.so.2`)
- `syscall` — comma-separated syscalls to redirect through their polyfill when called via `syscall()`

**Passthru caching:** The `wslcompat_passthru()` macro caches the result per callsite using file-static variables — avoids xattr re-parsing on every call.

## Vfork / CLONE_VM Safety

**Critical gotcha:** On `vfork`, parent and child share memory. The child MUST NOT mutate any shared state that the parent relies on.

**Solution: CoW override slots** (`synth.h` `struct synth_override`):
- Each `synth_entry` has `SYNTH_OVERRIDE_SLOTS` (4) override slots
- The installer process writes to the "base" field
- Non-installer processes claim slots keyed by `(pid, starttime)` — starttime prevents false matches on pid reuse
- `wslcompat_view_fd_load()` / `wslcompat_view_fd_store()` in synth.c are the generic accessors
- `tty.c` uses `wslcompat_view_fd_load()` and `wslcompat_view_fd_relocate()` for its `real_fd` via `tty_real_fd_load()`

Only the low 32 bits of `starttime` are stored. This is deliberate and safe: a false match needs pid reuse *and* a matching low-32-bit starttime wrap, which cannot happen in practice. Considered to death — do not raise it as a bug.

See `docs/VFORK_COW.md` for the full design rationale and the gdb+top bug that motivated it.

## TTY Synth Handler (tty.c)

The TTY handler is the most complex synth consumer because WSL1's tty driver doesn't honor non-canonical mode (VMIN/VTIME).

**How it works:**
1. On first `ioctl(TCSETS)`, `tty_install()` dups the user's real tty fd into `tty_state.real_fd`, then replaces the user's fd with a synth O_PATH wrap
2. All subsequent operations (read, ioctl, fcntl) go through the synth dispatch and operate on `real_fd`
3. The anchor problem: holding `real_fd` keeps the slave OFD alive, preventing cross-process master POLLHUP
4. The `close` shim's `post_close` uses a two-stage check to decide when to release `real_fd`:
   - Fast path: `flock(LOCK_EX)` on the probe fd succeeds → no other process has it → release
   - Fallback: walk `/proc/self/fd` to check if this process has another reference

See `docs/TTY_DESIGN.md` for the full design.

## Internal FD Registry (internal.c)

Libraries that hold long-lived fds can register them. When the application closes or dup2s over that fd number, the close/dup shims call `wslcompat_relocate_internal_fd()` to move the internal fd to a new number via `F_DUPFD_CLOEXEC` before the close executes.

Used by `logging.c` for the debug log fd. The slot convention: -2 = uninitialized, -1 = open failed, >= 0 = live fd.

## Locking Polyfill (fcntl.c)

WSL1's POSIX locks (`F_SETLK`) are broken (no cross-process blocking, no byte-range, no /proc/locks visibility). OFD locks (`F_OFD_SETLK`) return ENOSYS. BSD `flock()` works correctly.

**Solution:** All lock types (POSIX and OFD) are transparently upgraded to `flock()`. Limitations:
- Whole-file only (no byte-range)
- Multiple fds in the same process that don't share an OFD will conflict with each other
- `F_GETLK` reports conflicts even for same-process locks

## Logging

- Log macros: `wslerror`, `wslwarn`, `wslinfo`, `wsldebug`, `wsltrace`
- Output goes to `/dev/tty` by default (or `user.wslcompat.logfile` tunable)
- Level controlled by `user.wslcompat.debug` tunable (0-3)
- Uses `syscall(SYS_writev)` internally to avoid re-entering shims
- To capture log output: `script -c program /dev/stdout > logfile.txt`

## Testing

**Adding a new test:**
1. Create `tests/new_test.c`
2. Add it to the appropriate category in `tests/GNUmakefile`:
   - `TESTS` — must fail natively, pass with shim
   - `XPASS` — must pass both with and without shim
   - `XFAIL` — expected failure
   - `XFIXME` — known regression
3. Use `testutil.h` for WSL detection (`wslcompat_test_on_wsl()`, `wslcompat_test_require_or_skip()`)

**Test execution:** The Makefile automatically:
- Builds each test as `sys_<name>` (unpatched)
- Creates `fix_<name>` via `patchelf --add-needed` (patched)
- Runs via `LD_PRELOAD` as `pre_<name>`
- Runs under valgrind as `val_<name>`
- For `XPASS`: both `sys_` and `fix_` must pass
- For `TESTS`: `sys_` must fail (negated), `fix_`/`pre_`/`val_` must pass

**Linux compatibility:** `make linux` runs all tests natively (without the shim) to verify they pass on stock Linux. Tests that need WSL1-specific kernel behavior use `wslcompat_test_require_or_skip()` to skip gracefully.

## Naming & Style Conventions

- **Shim files** are named after the primary libc function they polyfill: `mmap.c`, `fcntl.c`, `open.c`
- **Internal headers** use `#ifndef WSLCOMPAT_*_H` guards
- **No comments** in source code unless documenting a non-obvious polyfill dependency
- **Error handling:** most shims use `goto cleanup` for resource cleanup on failure

## Gotchas

1. **Double-load safety**: If `LD_PRELOAD` contains the same library under two names, `ld.so` creates two link maps. `sym_next` can form cycles. Always use `syscall()` for internal bookkeeping, not `sym_next`.

2. **`open.c` includes `proc.c` directly** (not as a header). This is intentional — the proc handlers need access to open.c's static `dispatch` table and `wslcompat_synth_open`.

3. **The `O_TMPFILE` backing trick**: All synth fds are O_PATH refs to unlinked tempfiles. The kernel rejects most ops on O_PATH (EBADF/EPERM) — these are our canary signals to dispatch to the synth handler. Non-synth fds are completely unaffected.

4. **`__chk_fail` weak ref**: `shim.h` has a weak reference to glibc's `__chk_fail` for FORTIFY_SOURCE compatibility. The `fortify_chk_test` verifies this works.

5. **x86-64 + glibc only**: `RTLD_NEXT`, `gnu/libc-version.h`, `__chk_fail`, and the ELF64 fixup in `execve.c`/`elfclamp.c` are all x86-64/glibc-specific. Not portable to musl.

6. **epoll test skips on non-WSL**: The `epoll_synth_test` has `__attribute__((constructor))` that calls `exit(77)` (SKIP) on non-WSL kernels. Some tests may do similar early exits.
