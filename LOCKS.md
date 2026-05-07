# WSL1 File Locking

There are three standard facilities used for file locking, POSIX locks
`F_SETLK`, OFD locks `F_OFD_SETLK` and BSD locks `flock()`.

However, WSL1 only fully implements BSD locks. There is a partial
implementation of POSIX locks, but they are practically useless.

- Exclusive locks held by one process **do not block** another process from
  acquiring the same lock.
- No byte-range locking, the kernel silently rewrites any range to 0-4GB.
- No metadata, locks are not visible in /proc/locks and `F_GETLK` does not
  populate `l_pid`.

There is no implementation at all of OFD locks, attempting to use them will
return `ENOSYS`.

## Solution

This library transparently upgrades all lock requests (both POSIX and OFD) to
use `flock()`.

The `flock()` syscall on WSL1 is reasonably complete. It is the only primitive
on WSL1 that provides cross-process atomicity, and has similar semantics to OFD
locks.

## Limitations

This is obviously not perfect, there are some major problems. However, the
alternative is using broken POSIX locks.

- All locks are whole-file. Any application that expects to lock
  non-overlapping regions of the same file concurrently will be serialized
  (also true for POSIX locks on WSL1).
- Multiple file descriptors in the **same process** will conflict with each
  other if they don't share an OFD (i.e., they were opened via `open()` rather
  than `dup()`). This is stricter than native POSIX locks.
- `F_GETLK` will report a conflict even if the lock is held by the same
  process/descriptor that is querying.

