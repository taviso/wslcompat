
// Synthesize /proc/sys/vm/mmap_min_addr.
static int handle_mmap_min_addr(int dirfd, const char *pathname, int flags,
                                mode_t mode, int *out_fd)
{
    char buf[32];
    int n;

    // Calculate the value to expose.
    n = snprintf(buf, sizeof(buf), "%ld\n", wslcompat_tunable_int("mmap_min_addr", 4096));

    // Create an O_TMPFILE fd we can return.
    if ((*out_fd = open("/tmp", O_RDWR | O_TMPFILE, 0600)) < 0)
        return 0;

    wsldbg("generated synthetic fd %d for %s => '%s'", *out_fd, pathname, buf);

    // Write the value to it
    if (pwrite(*out_fd, buf, n, 0) != n) {
        close(*out_fd);
        return 0;
    }

    return 1;
}
