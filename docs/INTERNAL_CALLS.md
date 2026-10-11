# How libwslcompat calls libc and the kernel

Four patterns for invoking kernel-backed functions from inside the
library, NOT interchangeable. Picking the wrong one either bypasses a
polyfill or stack-overflows on double-load.

| Pattern | Use it when | Example |
|---|---|---|
| `syscall(SYS_foo, ...)` | Internal bookkeeping. Want kernel truth; your own shims aren't relevant. | constructors, `internal_fd` registry, `pin_fd`'s stat probe |
| `sym_next(foo, ...)` | Inside a shim body. Polyfill done, hand off to libc. | `int fstat(...)` -> `sym_next(fstat, fd, st)` |
| Bare `foo(...)` (libc name) | You deliberately want your own polyfill to fire. Document at call site. | `open("/tmp", O_TMPFILE, ...)` -- needs O_TMPFILE polyfill |
| Add a `case` to `syscall.c` | Polyfilling a syscall that has no libc wrapper. | hypothetical `kcmp` |

## Why `syscall()` for bookkeeping

The motivating failure:

```
$ LD_PRELOAD=./libwslcompat.so:libwslcompat.so bash -c true
Segmentation fault
```

`ld.so` deduplicates `LD_PRELOAD` entries by lookup name, not by file
identity. Relative path + bare name look distinct, so two link maps
get created for the same `.so`, both `-z interpose`.

`sym_next` then forms a chain that *can* cycle. The original cycle was
`fstat` and `pin_fd`: `fstat`'s body called `pin_fd`, which called
`sym_next(fstat)`. `RTLD_NEXT` skips the calling link map, so each hop
landed in the *other* library's shim, which called `pin_fd` again, and
so on until the stack ran out.

Replacing `sym_next(fstat)` with `syscall(SYS_fstat)` in `pin_fd` breaks
the cycle, because `syscall.c` is a pure passthrough: it does nothing
that could re-enter another shim. Chain terminates at libc.syscall ->
kernel after at most a couple of hops.

## Why `syscall.c` stays a pure passthrough

Row 1's safety depends on it. The moment a `case SYS_foo:` in
`syscall.c` does real work, any cycle passing through it becomes
another double-load stack overflow. The only legitimate addition is a
polyfill of a syscall that has *no libc wrapper to interpose*. If a
libc wrapper exists, polyfill the wrapper, not the syscall.

A single `wsltrace` at the top of `syscall()` is fine. Anything that
allocates, opens, or could re-enter a shim is not.

## Why bare libc name is an exception

`wslcompat_synth_open` opens a backing file via
`open("/tmp", O_TMPFILE, ...)`. We call the libc name on purpose
because WSL1's kernel doesn't implement O_TMPFILE and we need our own
`open` shim's polyfill to fire. `syscall(SYS_open, ...)` would skip
the polyfill and return EINVAL on WSL1.

Same idea for `dup3` and `close` in preexec save/restore: routed
through the shim so pre/post_close hooks fire and the synth slot's
refcount stays accurate.

Mark every such site with a one-line comment naming the polyfill it
depends on, so the next reader doesn't "fix" it into a `syscall()`
call.

## Checklist

- Constructor / `_init` / `_fini` -> `syscall(SYS_foo)`.
- Inside a shim body -> `sym_next(foo, ...)`.
- Internal helper called from a shim -> `syscall(SYS_foo)`.
- Need polyfill semantics from internal code -> bare `foo(...)` plus a
  comment.
- Adding to `syscall.c`'s switch -> check the syscall has no libc
  wrapper. If it does, write a shim for the wrapper instead.
