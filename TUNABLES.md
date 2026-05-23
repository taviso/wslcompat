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

Valid names: `clock_getres`, `clock_gettime`, `clock_nanosleep`, `execve`,
`execveat`, `fcntl`, `getsockopt`, `ioctl`, `mincore`, `mmap`, `open`,
`openat`, `renameat2`, `setsockopt`.

### `enabled`

Inverse of `disabled`. When set, *only* the listed polyfills are active;
anything not listed is disabled.

```
$ setfattr -n user.wslcompat.enabled -v "execve,execveat" $(which program)
```

### `debug`

Integer log level. Higher is more verbose; output goes to `/dev/tty`.

  * `0` - errors only (default)
  * `1` - also warnings
  * `2` - also info
  * `3` - also debug

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

### `taioffset`

Integer, default `37`.

The TAI–UTC offset in seconds used by the `CLOCK_TAI` polyfills.

```
$ setfattr -n user.wslcompat.taioffset -v 38 $(which program)
```

### `btime`

The creation time of a file as reported by `statx()`, usually set automatically.

You can set it manually on files, if you like:

```
$ setfattr -n user.wslcompat.btime -v "1700000000.000000000" /tmp/myfile
```

Files without this xattr fall back to a heuristic polyfill.
