/* Verify the internal-fd registry rescues our cached mount_fd when
 * the app closes whatever number it landed on.
 *
 * statx with STATX_MNT_ID triggers the polyfill in statx.c to open
 * /proc/self/mountinfo and cache the fd. We then walk /proc/self/fd
 * looking for any fd whose readlink ends in "/mountinfo", close it,
 * and run statx again. If the registry's relocate path works, the
 * second call returns the same mnt_id; if it doesn't, the cached fd
 * is dead and the polyfill returns 0.
 *
 * On native Linux the kernel provides STATX_MNT_ID directly and our
 * polyfill (and mount_fd) never run -- the second statx still works
 * for the same reason as the first, so this passes unshimmed too. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/stat.h>

#ifndef STATX_MNT_ID
# define STATX_MNT_ID 0x1000U
#endif

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

// Scan /proc/self/fd; collect, then close, any fd whose readlink ends
// in "/mountinfo". Collect-first-close-after so we don't perturb the
// readdir cursor. Returns the count closed, or -1 if /proc isn't there.
static int close_mountinfo_fds(void)
{
    int fds[64];
    int n = 0;

    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;

    struct dirent *e;
    while ((e = readdir(d)) && n < (int)(sizeof(fds) / sizeof(fds[0]))) {
        if (e->d_name[0] == '.')
            continue;

        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);

        char link[256];
        ssize_t r = readlink(path, link, sizeof(link) - 1);
        if (r <= 0)
            continue;
        link[r] = '\0';

        int fd = atoi(e->d_name);
        if (fd == dirfd(d))
            continue;

        const char *slash = strrchr(link, '/');
        if (slash && strcmp(slash, "/mountinfo") == 0)
            fds[n++] = fd;
    }
    closedir(d);

    int closed = 0;
    for (int i = 0; i < n; i++) {
        if (close(fds[i]) == 0)
            closed++;
    }
    return closed;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    struct statx stx1 = {0};
    struct statx stx2 = {0};

    if (statx(AT_FDCWD, "/etc", 0, STATX_MNT_ID, &stx1) != 0) {
        printf("FAIL: first statx failed: %m\n");
        return 1;
    }
    CHECK(stx1.stx_mask & STATX_MNT_ID,
          "first statx provided STATX_MNT_ID");
    CHECK(stx1.stx_mnt_id != 0,
          "first mnt_id %llu is nonzero",
          (unsigned long long)stx1.stx_mnt_id);

    // Closing here triggers the registry's relocate path when our
    // shim is active. Without the shim there are usually zero
    // mountinfo fds to close.
    int closed = close_mountinfo_fds();
    CHECK(closed >= 0, "scanned /proc/self/fd successfully");
    printf("  INFO: closed %d mountinfo fd(s)\n", closed);

    if (statx(AT_FDCWD, "/etc", 0, STATX_MNT_ID, &stx2) != 0) {
        printf("FAIL: second statx failed: %m\n");
        return 1;
    }
    CHECK(stx2.stx_mask & STATX_MNT_ID,
          "second statx provided STATX_MNT_ID");
    CHECK(stx2.stx_mnt_id == stx1.stx_mnt_id,
          "second mnt_id (%llu) matches first (%llu)",
          (unsigned long long)stx2.stx_mnt_id,
          (unsigned long long)stx1.stx_mnt_id);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
