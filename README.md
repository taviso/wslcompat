# wslcompat

This is an experimental library to workaround incompatibilities in WSL1.

The idea is to patch binaries that use unimplemented functionality with
"polyfills", user-space implementations of the missing functionality.

## List of polyfills

- `MAP_FIXED_NOREPLACE` is unimplemented
- `getsockopt(SO_PROTOCOL)` is unimplemented for `AF_UNIX`
- `getsockopt(SO_DOMAIN)` is unimplemented for `AF_UNIX`
- `mincore()` is unimplemented
- `F_OFD_SETLK`/``F_OFD_GETLK` is unimplemented.

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
