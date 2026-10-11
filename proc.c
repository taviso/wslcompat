
// Render /proc/sys/vm/mmap_min_addr on demand; e->pos owned by us.
static ssize_t mmap_min_addr_read(struct synth_entry *e,
                                  void *buf, size_t count)
{
    char tmp[32];
    int len;
    size_t take;

    len = snprintf(tmp, sizeof(tmp), "%ld\n",
                   wslcompat_tunable_int("mmap_min_addr", 4096));

    if (e->pos >= len)
        return 0;

    take = count;
    if (take > (size_t)(len - e->pos))
        take = len - e->pos;

    memcpy(buf, tmp + e->pos, take);
    e->pos += take;
    return take;
}

static off_t mmap_min_addr_lseek(struct synth_entry *e,
                                 off_t offset, int whence)
{
    off_t new_pos;

    // Linux procfs files typically report size 0, so SEEK_END acts like SEEK_SET 0
    if (whence == SEEK_SET || whence == SEEK_END) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        if (__builtin_add_overflow(e->pos, offset, &new_pos))
            return errno = EINVAL, -1;
    } else {
        return errno = EINVAL, -1;
    }

    if (new_pos < 0)
        return errno = EINVAL, -1;

    e->pos = new_pos;
    return new_pos;
}

static ssize_t mmap_min_addr_pread(struct synth_entry *e,
                                   void *buf, size_t count, off_t offset)
{
    char tmp[32];
    int len;
    size_t take;

    if (offset < 0)
        return errno = EINVAL, -1;

    len = snprintf(tmp, sizeof(tmp), "%ld\n", wslcompat_tunable_int("mmap_min_addr", 4096));

    if (offset >= len)
        return 0;

    take = count;
    if (take > (size_t)(len - offset))
        take = len - offset;

    memcpy(buf, tmp + offset, take);
    return take;
}

static int mmap_min_addr_describe(struct synth_entry *e,
                                  char *buf, size_t size)
{
    (void)e;
    return snprintf(buf, size, "/proc/sys/vm/mmap_min_addr polyfill");
}

static const struct synth_ops mmap_min_addr_ops = {
    .read     = mmap_min_addr_read,
    .pread    = mmap_min_addr_pread,
    .lseek    = mmap_min_addr_lseek,
    .describe = mmap_min_addr_describe,
};

// Mint an O_PATH fd backed by mmap_min_addr_ops; read() shim routes
// EBADF to the handler.
static int handle_mmap_min_addr(int dirfd, const char *pathname, int flags,
                                mode_t mode, int *out_fd)
{
    // Always claim: declining on failure would let the kernel clobber
    // synth_open's errno (e.g. ENFILE).
    *out_fd = wslcompat_synth_open(flags, &mmap_min_addr_ops, NULL);
    wsldbg("synthesized fd=%d for %s", *out_fd, pathname);
    return 1;
}

// /proc/<pid>/mem polyfill via process_vm_readv/writev with pos as
// the target VA. pid captured at open time in e->userptr.
static ssize_t proc_pid_mem_read(struct synth_entry *e,
                                 void *buf, size_t count)
{
    struct iovec local  = { .iov_base = buf,
                            .iov_len  = count };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)e->pos,
                            .iov_len  = count };
    ssize_t n;

    n = process_vm_readv((pid_t)(uintptr_t)e->userptr,
                         &local, 1, &remote, 1, 0);
    if (n > 0)
        e->pos += n;
    return n;
}

static ssize_t proc_pid_mem_write(struct synth_entry *e,
                                  const void *buf, size_t count)
{
    struct iovec local  = { .iov_base = (void *)buf,
                            .iov_len  = count };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)e->pos,
                            .iov_len  = count };
    ssize_t n;

    n = process_vm_writev((pid_t)(uintptr_t)e->userptr,
                          &local, 1, &remote, 1, 0);
    if (n > 0)
        e->pos += n;
    return n;
}

static ssize_t proc_pid_mem_pread(struct synth_entry *e,
                                  void *buf, size_t count, off_t offset)
{
    struct iovec local  = { .iov_base = buf,
                            .iov_len  = count };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)offset,
                            .iov_len  = count };

    return process_vm_readv((pid_t)(uintptr_t)e->userptr,
                            &local, 1, &remote, 1, 0);
}

static ssize_t proc_pid_mem_pwrite(struct synth_entry *e,
                                   const void *buf, size_t count, off_t offset)
{
    struct iovec local  = { .iov_base = (void *)buf,
                            .iov_len  = count };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)offset,
                            .iov_len  = count };

    return process_vm_writev((pid_t)(uintptr_t)e->userptr,
                             &local, 1, &remote, 1, 0);
}

static ssize_t proc_pid_mem_preadv(struct synth_entry *e,
                                   const struct iovec *iov, int iovcnt,
                                   off_t offset)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (__builtin_add_overflow(total, iov[i].iov_len, &total))
            return errno = EINVAL, -1;
    }
    if (total > SSIZE_MAX)
        return errno = EINVAL, -1;

    struct iovec remote = { .iov_base = (void *)(uintptr_t)offset,
                            .iov_len  = total };

    return process_vm_readv((pid_t)(uintptr_t)e->userptr,
                            iov, iovcnt, &remote, 1, 0);
}

static ssize_t proc_pid_mem_pwritev(struct synth_entry *e,
                                    const struct iovec *iov, int iovcnt,
                                    off_t offset)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (__builtin_add_overflow(total, iov[i].iov_len, &total))
            return errno = EINVAL, -1;
    }
    if (total > SSIZE_MAX)
        return errno = EINVAL, -1;

    struct iovec remote = { .iov_base = (void *)(uintptr_t)offset,
                            .iov_len  = total };

    return process_vm_writev((pid_t)(uintptr_t)e->userptr,
                             iov, iovcnt, &remote, 1, 0);
}

static ssize_t proc_pid_mem_readv(struct synth_entry *e,
                                  const struct iovec *iov, int iovcnt)
{
    ssize_t n = proc_pid_mem_preadv(e, iov, iovcnt, e->pos);
    if (n > 0)
        e->pos += n;
    return n;
}

static ssize_t proc_pid_mem_writev(struct synth_entry *e,
                                   const struct iovec *iov, int iovcnt)
{
    ssize_t n = proc_pid_mem_pwritev(e, iov, iovcnt, e->pos);
    if (n > 0)
        e->pos += n;
    return n;
}

// SEEK_END has no meaning for an address space -- reject.
static off_t proc_pid_mem_lseek(struct synth_entry *e,
                                off_t offset, int whence)
{
    off_t new_pos;

    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        if (__builtin_add_overflow(e->pos, offset, &new_pos))
            return errno = EINVAL, -1;
    } else {
        return errno = EINVAL, -1;
    }

    if (new_pos < 0)
        return errno = EINVAL, -1;

    e->pos = new_pos;
    return new_pos;
}

static int proc_pid_mem_describe(struct synth_entry *e,
                                 char *buf, size_t size)
{
    pid_t pid = (pid_t)(uintptr_t)e->userptr;
    return snprintf(buf, size, "/proc/%d/mem polyfill", pid);
}

static const struct synth_ops proc_pid_mem_ops = {
    .read     = proc_pid_mem_read,
    .write    = proc_pid_mem_write,
    .pread    = proc_pid_mem_pread,
    .pwrite   = proc_pid_mem_pwrite,
    .readv    = proc_pid_mem_readv,
    .writev   = proc_pid_mem_writev,
    .preadv   = proc_pid_mem_preadv,
    .pwritev  = proc_pid_mem_pwritev,
    .lseek    = proc_pid_mem_lseek,
    .describe = proc_pid_mem_describe,
};

// Resolve the <X> in /proc/<X>/... to a pid. <X> is either "self" or
// a decimal pid; returns -1 otherwise.
static pid_t proc_parse_pid(const char *p)
{
    if (strncmp(p, "self/", 5) == 0)
        return getpid();

    char *end;
    long v = strtol(p, &end, 10);
    if (*end != '/' || v <= 0 || v > INT_MAX)
        return -1;
    return (pid_t)v;
}

static int handle_proc_pid_mem(int dirfd, const char *pathname, int flags,
                               mode_t mode, int *out_fd)
{
    // fnmatch dispatch guarantees /proc/<X>/mem or /proc/<X>/task/<Y>/mem.
    // <Y> is ignored (all threads share VM).
    pid_t pid = proc_parse_pid(pathname + sizeof("/proc/") - 1);
    if (pid <= 0)
        return 0;

    *out_fd = wslcompat_synth_open(flags, &proc_pid_mem_ops,
                                   (void *)(uintptr_t)pid);
    wsldbg("synthesized fd=%d for %s (pid=%d)", *out_fd, pathname, pid);
    return 1;
}
