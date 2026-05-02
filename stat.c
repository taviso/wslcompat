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

static void __attribute__((constructor)) init(void) {
    sym_statx = dlsym(RTLD_NEXT, "statx");
}

int statx(int dirfd, const char *pathname, int flags,
          unsigned int mask, struct statx *statxbuf) {
    // Pass through the call to glibc
    int ret = sym_statx(dirfd, pathname, flags, mask, statxbuf);

    // If it failed, no need to do anything.
    if (ret != 0)
        return ret;

    // Check if caller wanted STATX_MNT_ID
    if ((mask & STATX_MNT_ID) == 0)
        return ret;

    // Check if glibc provided it (probably not, but maybe they will in future)
    if (statxbuf->stx_mask & STATX_MNT_ID)
        return ret;

    // Okay, we have to provide it ourselves.
    statxbuf->stx_mnt_id = lookup_mnt_id(statxbuf->stx_dev_major, statxbuf->stx_dev_minor);

    // If it worked, fix the mask.
    if (statxbuf->stx_mnt_id) {
        statxbuf->stx_mask |= STATX_MNT_ID;
    }

    return ret;
}
