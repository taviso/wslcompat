# wslcompat

wslcompat is a compatibility layer that enables more software to work on WSL1.

It provides "polyfills" for missing system calls and features, filling the gaps
that prevent your software from running. This doesn't require any changes to
the binaries or the underlying kernel.

## Building

Just type `make`, then `make install`.

## Usage

First, verify that this library will fix your program

```
$ LD_PRELOAD=/usr/local/lib/libwslcompat.so program
```

If that works, you can make the change permanent

```
$ patchelf --add-needed /usr/local/lib/libwslcompat.so $(which program)
```

> Note: The `patchelf` utility is available in most package managers.

If this doesn't fix your binary, or causes any problems, please open an issue.

## Scripts

If you're trying to fix a script, you need to `patchelf` the interpreter, such
as `python` or `ruby`.

For example, the `multiprocessing` python module does not work on WSL1:

```
$ python multiproc.py
Traceback (most recent call last):
  File "<string>", line 1, in <module>
  File "/usr/lib/python3.12/multiprocessing/forkserver.py", line 207, in main
    with socket.socket(socket.AF_UNIX, fileno=listener_fd) as listener, \
         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  File "/usr/lib/python3.12/socket.py", line 233, in __init__
    _socket.socket.__init__(self, family, type, proto, fileno)
OSError: [Errno 22] Invalid argument
```

However, we correctly polyfill the necessary socket options.

```
$ LD_PRELOAD=libwslcompat.so python multiproc.py
hello
```

If you want this to be permanent, simply try this:

```
$ sudo patchelf --add-needed libwslcompat.so /usr/bin/python
```

## Systemwide Preloading

This library supports preloading via `/etc/ld.so.preload`, making it apply
systemwide.

```
$ echo /usr/local/lib/libwslcompat.so | sudo tee -a /etc/ld.so.preload
```

If necessary, you can use [tunables](#tunables) to configure individual
programs, or set the `__WSLCOMPAT_DISABLE` environment variable.

## Testing

There are a variety of tests in the tests directory that verify the polyfills
are functioning.

Type `make test` to run them.

## Available Polyfills

- `MAP_FIXED_NOREPLACE` is unimplemented.
- `getsockopt(SO_PROTOCOL)` is unimplemented for `AF_UNIX`.
- `getsockopt(SO_DOMAIN)` is unimplemented for `AF_UNIX`.
- `mincore()` is unimplemented.
- `F_OFD_SETLK`/`F_OFD_GETLK` are unimplemented.
- `F_SETLK` exclusive locks are process scoped.
- `VMIN` and `VTIME` are ignored by non-canonical terminals.
- `STATX_MNT_ID` is unimplemented.
- `STATX_ATTR_MOUNT_ROOT` is unimplemented.
- `STATX_BTIME` is unimplemented.
- `MAP_LOCKED` is unimplemented.
- `RENAME_NOREPLACE` is unimplemented.
- `execve()` rejects ELF64 binaries with mixed `PT_LOAD` `p_align`.
- `execveat()` is unimplemented.
- `clock_nanosleep()` rejects `CLOCK_BOOTTIME`, `CLOCK_TAI`, and `CLOCK_PROCESS_CPUTIME_ID`.
- `clock_getres()` and `clock_gettime()` reject `CLOCK_TAI`.
- `clock_gettime()` rejects the encoded clockids from `clock_getcpuclockid()`.
- `SO_REUSEPORT` is accepted but is unimplemented.
- `O_TMPFILE` is unimplemented.

## Tunables

You can configure wslcompat using extended attributes, called tunables.

Tunables let you change the behavior of each binary by enabling or disabling
polyfills and features, which is particularly useful when wslcompat is
loaded via `/etc/ld.so.preload`.

For the full reference of every tunable and how to set them, see
[TUNABLES.md](TUNABLES.md).

A few common ones:

```
$ setfattr -n user.wslcompat.disabled -v "fcntl,mmap" $(which program)
$ setfattr -n user.wslcompat.debug -v 2 $(which program)
```

## File Locking

The file locking primitives available on WSL1 are extremely limited.

This library makes an attempt to improve the consistency of locking, but does
so by mapping all lock types onto the one reliable locking mechanism.

For further discussion on the problem please see [LOCKS.md](LOCKS.md).

## Elf64 Loading

The Elf64 loader in WSL1 will reject any binary that has `PT_LOAD` program
headers with non-uniform alignment. These are perfectly valid and common
programs, so an `execve` polyfill attempts to detect this case.

The polyfill can re-exec binaries via their interpreter, but this only works
for dynamically linked binaries.

For static binaries, you can use `elfclamp` to patch the binary in place.

```
$ elfclamp /path/to/program
```

This unifies every `PT_LOAD` `p_align` to the smallest value observed, which
preserves the ELF congruence rule. Already-uniform binaries are left alone.

## Future

### Polyfills

We can polyfill these in future.

- `kcmp`
    - For the `pid1`==`pid2` and `KCMP_FILE` case, we can use toggle flags with
      `F_GETFL`/`F_SETFL` to see if a file is the same.
- `SO_TIMESTAMP` on `AF_UNIX`
    - Needs `setsockopt`/`getsockopt` to track per-fd state and `recvmsg` to
      splice an `SCM_TIMESTAMP` cmsg captured around the underlying recv.
- `setitimer(ITIMER_PROF)` and `setitimer(ITIMER_VIRTUAL)`
    - WSL1 rejects both with `EINVAL`, may require a helper thread.
- `linkat` on an `O_TMPFILE` fd

### Features

- Path redirection via a `user.wslcompat.redir` tunable?
