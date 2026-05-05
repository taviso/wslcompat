#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/stat.h>
#include <errno.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

// A cached file descriptor to /proc/self/mountinfo
static int mount_fd = -1;

static int (*sym_statx)(int dirfd, const char *pathname, int flags,
                        unsigned int mask, struct statx *statxbuf);

static void __attribute__((destructor)) fini(void) {
    close(mount_fd);
}

static uint64_t lookup_mnt_id(uint32_t maj, uint32_t min) {
    char buf[8192];
    char *curr = buf;
    ssize_t n;

    // Thread-safe lazy open.
    if (__builtin_expect(__sync_add_and_fetch(&mount_fd, 0) == -1, 0)) {
        int fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);

        if (fd == -1) return 0;

        if (!__sync_bool_compare_and_swap(&mount_fd, -1, fd)) {
            close(fd);
        }
    }

    // pread is atomic and does not share an offset with other threads.
    if ((n = pread(mount_fd, buf, sizeof(buf) - 1, 0)) <= 0)
        return 0;

    buf[n] = '\0';

    while (curr && *curr) {
        char *p;
        uint64_t id;
        uint32_t r_maj, r_min;

        // The format is: mnt_id parent_id major:minor ...
        id = strtoul(curr, &p, 10);

        // Skip over parent_id
        strtoul(p, &p, 10);

        r_maj = strtoul(p, &p, 10);

        // Skip over ':'
        if (*p++ != ':')
            return 0;

        r_min = strtoul(p, &p, 10);

        if (r_maj == maj && r_min == min) {
            return id;
        }

        // Advance to the next line
        if ((curr = strchr(p, '\n')))
            curr++;
    }

    return 0;
}

static int is_mount_root(int dirfd, const char *pathname, int flags, struct statx *stx) {
    struct stat parent;
    char buf[PATH_MAX];

    // If it's not a directory, it's not a mount root.
    // We forced STATX_TYPE in the caller.
    if (!(stx->stx_mask & STATX_TYPE) || !S_ISDIR(stx->stx_mode))
        return 0;

    // We forced STATX_INO in the caller.
    if (!(stx->stx_mask & STATX_INO))
        return 0;

    uint64_t dev = makedev(stx->stx_dev_major, stx->stx_dev_minor);
    uint64_t ino = stx->stx_ino;

    // Identify the parent.
    if (!pathname || !*pathname || (flags & AT_EMPTY_PATH)) {
        // Parent is simply ".." relative to the FD.
        if (fstatat(dirfd, "..", &parent, flags & AT_NO_AUTOMOUNT) != 0)
            return 0;
    } else {
        // Directory case: "path/.." is the most robust way to find the parent.
        if (snprintf(buf, sizeof(buf), "%s/..", pathname) >= sizeof(buf))
            return 0;
        if (fstatat(dirfd, buf, &parent, flags & AT_NO_AUTOMOUNT) != 0)
            return 0;
    }

    // It's a mount root if device IDs differ or it's the global root (ino == parent).
    return (dev != parent.st_dev) || (ino == parent.st_ino);
}

static void __attribute__((constructor)) init(void) {
    sym_statx = dlsym(RTLD_NEXT, "statx");
}

int statx(int dirfd, const char *pathname, int flags,
          unsigned int mask, struct statx *statxbuf) {
    // Pass through the call to glibc.
    // We force STATX_INO and STATX_TYPE so is_mount_root can reuse them.
    // We also force STATX_MTIME and STATX_CTIME for BTIME polyfill.
    int ret = sym_statx(dirfd, pathname, flags, mask | STATX_INO | STATX_TYPE | STATX_MTIME | STATX_CTIME, statxbuf);

    // If it failed, no need to do anything.
    if (ret != 0)
        return ret;

    // Check if caller wanted STATX_MNT_ID
    if ((mask & STATX_MNT_ID) && !(statxbuf->stx_mask & STATX_MNT_ID)) {

        // Okay, we have to provide it ourselves.
        statxbuf->stx_mnt_id = lookup_mnt_id(statxbuf->stx_dev_major, statxbuf->stx_dev_minor);

        // If it worked, fix the mask.
        if (statxbuf->stx_mnt_id) {
            statxbuf->stx_mask |= STATX_MNT_ID;
        }
    }

    // Polyfill STATX_ATTR_MOUNT_ROOT if missing from mask
    if (!(statxbuf->stx_attributes_mask & STATX_ATTR_MOUNT_ROOT)) {
        if (is_mount_root(dirfd, pathname, flags, statxbuf)) {
            statxbuf->stx_attributes |= STATX_ATTR_MOUNT_ROOT;
        }
        statxbuf->stx_attributes_mask |= STATX_ATTR_MOUNT_ROOT;
    }

    // Polyfill STATX_BTIME if requested but missing
    if ((mask & STATX_BTIME) && !(statxbuf->stx_mask & STATX_BTIME)) {
        if ((statxbuf->stx_mask & STATX_MTIME) && (statxbuf->stx_mask & STATX_CTIME)) {
            // Use the earlier of mtime or ctime as a birth time heuristic.
            if (statxbuf->stx_mtime.tv_sec < statxbuf->stx_ctime.tv_sec ||
               (statxbuf->stx_mtime.tv_sec == statxbuf->stx_ctime.tv_sec &&
                statxbuf->stx_mtime.tv_nsec < statxbuf->stx_ctime.tv_nsec)) {
                statxbuf->stx_btime = statxbuf->stx_mtime;
            } else {
                statxbuf->stx_btime = statxbuf->stx_ctime;
            }
            statxbuf->stx_mask |= STATX_BTIME;
        }
    }

    // Restore the mask to what the user actually requested.
    if (!(mask & STATX_INO))
        statxbuf->stx_mask &= ~STATX_INO;
    if (!(mask & STATX_TYPE))
        statxbuf->stx_mask &= ~STATX_TYPE;
    if (!(mask & STATX_MTIME))
        statxbuf->stx_mask &= ~STATX_MTIME;
    if (!(mask & STATX_CTIME))
        statxbuf->stx_mask &= ~STATX_CTIME;

    return ret;
}
