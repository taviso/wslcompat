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

## Testing

There are a variety of tests in the tests directory that verify the polyfills
are functioning.

Type `make test` to run them.

## List of polyfills

- `MAP_FIXED_NOREPLACE` is unimplemented
- `getsockopt(SO_PROTOCOL)` is unimplemented for `AF_UNIX`
- `getsockopt(SO_DOMAIN)` is unimplemented for `AF_UNIX`
- `getsockopt(SO_TIMESTAMP)` is unimplemented for `AF_UNIX`
- `mincore()` is unimplemented
- `F_OFD_SETLK`/`F_OFD_GETLK` is unimplemented.
- `VMIN` and `VTIME` are ignored by non-canonical terminals.
- `STATX_MNT_ID` is unimplemented.
- `STATX_ATTR_MOUNT_ROOT` is unimplemented.

