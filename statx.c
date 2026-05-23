#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <linux/stat.h>
#include <errno.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "shim.h"
#include "logging.h"
#include "tunables.h"

#ifndef STATX_ATTR_MOUNT_ROOT
# define STATX_ATTR_MOUNT_ROOT 0x2000
#endif
#ifndef STATX_MNT_ID
# define STATX_MNT_ID 0x1000U
#endif

SHIM_INIT(statx);

// A cached file descriptor to /proc/self/mountinfo
static int mount_fd = -1;

static void __attribute__((destructor)) fini(void) {
    if (mount_fd != -1)
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
    if (!pathname || !*pathname) {
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

static struct statx_timestamp stx_ts_min(struct statx_timestamp a,
                                         struct statx_timestamp b)
{
    if (a.tv_sec < b.tv_sec)
        return a;

    if (b.tv_sec < a.tv_sec)
        return b;

    if (a.tv_nsec < b.tv_nsec)
        return a;
    return b;
}

// Read the user.wslcompat.btime xattr (written by open on O_CREAT).
static int read_btime_xattr(int dirfd, const char *pathname, int flags,
                            struct statx_timestamp *out)
{
    ssize_t n;
    char path[PATH_MAX];
    char buf[64] = {0};

    if (!pathname)
        return -1;

    if (*pathname != '\0') {
        const char *target = pathname;
        // Figure out how to reference the file via /proc.
        if (pathname[0] != '/' && dirfd != AT_FDCWD) {
            snprintf(path, sizeof(path), "/proc/self/fd/%d/%s", dirfd, pathname);
            target = path;
        }

        if (flags & AT_SYMLINK_NOFOLLOW)
            n = lgetxattr(target, "user.wslcompat.btime", buf, sizeof(buf) - 1);
        else
            n = getxattr(target, "user.wslcompat.btime", buf, sizeof(buf) - 1);

        if (n < 0) {
            wsldbg("failed to query btime xattr of %s, %m", target);
            return -1;
        }
    } else if (dirfd == AT_FDCWD) {
        if (getxattr(".", "user.wslcompat.btime", buf, sizeof(buf) - 1) < 0) {
            wsldbg("failed to query btime xattr of cwd, %m");
            return -1;
        }
    } else {
        if (fgetxattr(dirfd, "user.wslcompat.btime", buf, sizeof(buf) - 1) < 0) {
            wsldbg("failed to query btime xattr of fd=%d, %m", dirfd);
            return -1;
        }
    }

    if (sscanf(buf, "%lld.%u", &out->tv_sec, &out->tv_nsec) != 2) {
        wslwarn("is the btime attribute on file %s corrupt?", pathname);
        return -1;
    }

    return 0;
}

int statx(int dirfd, const char *pathname, int flags,
          unsigned int mask, struct statx *statxbuf) {

    int ret;
    unsigned forced;

    // Check this shim is enabled.
    if (wslcompat_passthru_self())
        return sym_next(statx, dirfd, pathname, flags, mask, statxbuf);

    // These are flags we force on that we need for our polyfill.
    forced = STATX_INO | STATX_TYPE | STATX_MTIME | STATX_CTIME;

    // Pass through the call.
    if ((ret = sym_next(statx, dirfd, pathname, flags, mask | forced, statxbuf)) != 0)
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

    // Polyfill STATX_BTIME if requested but missing. Prefer the xattr
    // written by the open shim at creation time; fall back to the
    // earlier-of-mtime/ctime heuristic if absent.
    if ((mask & STATX_BTIME) && !(statxbuf->stx_mask & STATX_BTIME)) {
        // This library records btime when possible, see if we set it.
        if (read_btime_xattr(dirfd, pathname, flags, &statxbuf->stx_btime) == 0) {
            statxbuf->stx_mask |= STATX_BTIME;
        } else if ((statxbuf->stx_mask & STATX_MTIME) && (statxbuf->stx_mask & STATX_CTIME)) {
            wsldbg("No btime recorded, using the earlier of mtime or ctime as heuristic.");
            statxbuf->stx_btime = stx_ts_min(statxbuf->stx_mtime, statxbuf->stx_ctime);
            statxbuf->stx_mask |= STATX_BTIME;
        }
    }

    // Clear bits we forced but the caller didn't actually request.
    statxbuf->stx_mask &= ~(forced & ~mask);

    return ret;
}
