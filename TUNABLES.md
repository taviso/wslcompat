# wslcompat tunables

wslcompat reads its configuration from extended attributes (xattrs).

Each tunable lives under the `user.wslcompat.` namespace, and applies per-binary.

## Setting and reading tunables

Set a value:

```
$ setfattr -n user.wslcompat.NAME -v VALUE /path/to/program
```

Read a value:

```
$ getfattr -n user.wslcompat.NAME /path/to/program
```

Dump every xattr on a binary:

```
$ getfattr -d /path/to/program
```

Remove a tunable:

```
$ setfattr -x user.wslcompat.NAME /path/to/program
```

## Available tunables

### `disabled`

Comma-separated list of polyfills to skip.

```
$ setfattr -n user.wslcompat.disabled -v "fcntl,mmap" $(which program)
```

Valid names: `execve`, `execveat`, `fcntl`, `ioctl`, `mincore`, `mmap`,
`renameat2`.

### `enabled`

Inverse of `disabled`. When set, *only* the listed polyfills are active;
anything not listed is disabled.

```
$ setfattr -n user.wslcompat.enabled -v "execve,execveat" $(which program)
```

### `debug`

Integer log level. Higher is more verbose; output goes to `/dev/tty`.

  * `0` — errors only (default)
  * `1` — also warnings
  * `2` — also info
  * `3` — also debug

```
$ setfattr -n user.wslcompat.debug -v 2 $(which program)
```

### `argv0`

Boolean, default `true`.

This tunable controls whether the `execve` polyfill passes the caller's
`argv[0]` through to ld-linux's `--argv0` flag when re-executing a binary.

> You may need to set this on systems with `glibc` < 2.33.

```
$ setfattr -n user.wslcompat.argv0 -v 0 $(which program)
```

### `ptinterp`

String, default `/lib64/ld-linux-x86-64.so.2`.

The path of the dynamic loader the `execve` polyfill uses to re-exec binaries
with mixed `PT_LOAD` `p_align`. Override if your distribution keeps its loader
elsewhere.

```
$ setfattr -n user.wslcompat.ptinterp -v /lib/ld-linux.so.2 $(which program)
```
