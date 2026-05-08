# wslcompat

This is an experimental library to workaround incompatibilities in WSL1.

The idea is to patch binaries that use unimplemented functionality with
"polyfills", user-space implementations of the missing functionality.

## Building

Just type `make`, then copy `libwslcompat.so` to `/usr/local/lib/`.

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

For example, The `multiprocessing` python module does not work on WSL1:

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

## Testing

There are a variety of tests in the tests directory that verify the polyfills
are functioning.

Type `make test` to run them.

## Available Polyfills

- `MAP_FIXED_NOREPLACE` is unimplemented
- `getsockopt(SO_PROTOCOL)` is unimplemented for `AF_UNIX`
- `getsockopt(SO_DOMAIN)` is unimplemented for `AF_UNIX`
- `getsockopt(SO_TIMESTAMP)` is unimplemented for `AF_UNIX`
- `mincore()` is unimplemented
- `F_OFD_SETLK`/`F_OFD_GETLK` is unimplemented.
- `F_SETLK` exclusive locks are process scoped.
- `VMIN` and `VTIME` are ignored by non-canonical terminals.
- `STATX_MNT_ID` is unimplemented.
- `STATX_ATTR_MOUNT_ROOT` is unimplemented.
- `STATX_BTIME` is unimplemented.
- `MAP_LOCKED` is unimplemented.
- `RENAME_NOREPLACE` is unimplemented.

## File Locking

The file locking primitives available on WSL1 are extremely limited.

This library makes an attempt to improve the consistency of locking, but does
so by mapping all lock types onto the one reliable locking mechanism.

For further discussion on the problem please see [LOCKS.md](LOCKS.md).

## Future

### Polyfills

We can polyfill these in future.

- `kcmp`
    - For the `pid1`==`pid2` and `KCMP_FILE` case, we can use toggle flags with
      `F_GETFL`/`F_SETFL` to see if a file is the same.

### Features

- Should be able to selectively adjust features and locking via glibc-like tunables?
