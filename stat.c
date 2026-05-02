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

static int (*sym_statx)(int dirfd, const char *pathname, int flags,
                        unsigned int mask, struct statx *statxbuf);

static uint64_t lookup_mnt_id(uint32_t maj, uint32_t min) {
    FILE *fp = fopen("/proc/self/mountinfo", "re");
    unsigned long long id, m, n;

    // If that failed, we can't check.
    if (fp == NULL)
        return 0;

    // Find the matching mount.
    while (fscanf(fp, "%llu %*u %llu:%llu %*[^\n] ", &id, &m, &n) == 3) {
        if (m == maj && n == min) {
            fclose(fp);
            return id;
        }
    }

    // No match
    fclose(fp);
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
    int ret = sym_statx(dirfd, pathname, flags, mask | STATX_INO | STATX_TYPE, statxbuf);

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

    // Restore the mask to what the user actually requested.
    if (!(mask & STATX_INO))
        statxbuf->stx_mask &= ~STATX_INO;
    if (!(mask & STATX_TYPE))
        statxbuf->stx_mask &= ~STATX_TYPE;

    return ret;
}
