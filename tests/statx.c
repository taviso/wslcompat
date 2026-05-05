#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/stat.h>
#include <errno.h>
#include <err.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifndef STATX_ATTR_MOUNT_ROOT
# define STATX_ATTR_MOUNT_ROOT 0x2000
#endif

int check_stx_mask(struct statx *stx, const char *name, int flag)
{
    if (stx->stx_mask & flag) {
        printf("stx_mask *did* provide %s\n", name);
        return 1;
    }
    printf("stx_mask *did not* provide %s\n", name);
    return 0;
}

int check_stx_attribute(struct statx *stx, const char *name, uint64_t flag)
{
    if (stx->stx_attributes_mask & flag) {
        if (stx->stx_attributes & flag) {
            printf("stx_attributes *did* have %s set\n", name);
            return 1;
        }
        printf("stx_attributes *did not* have %s set\n", name);
        return 0;
    }

    printf("stx_attributes_mask *did not* include %s\n", name);
    return 0;
}

uint64_t get_expected_mnt_id(uint32_t major, uint32_t minor)
{
    FILE *fp = fopen("/proc/self/mountinfo", "re");
    unsigned long long id;
    unsigned int m, n;

    if (!fp) return 0;

    while (fscanf(fp, "%llu %*u %u:%u %*[^\n] ", &id, &m, &n) == 3) {
        if (m == major && n == minor) {
            fclose(fp);
            return id;
        }
    }

    fclose(fp);
    return 0;
}

void verify_mnt_id(struct statx *stx)
{
    uint64_t expected = get_expected_mnt_id(stx->stx_dev_major, stx->stx_dev_minor);

    if (stx->stx_mnt_id != expected) {
        errx(EXIT_FAILURE, "stx_mnt_id mismatch: expected %llu, got %llu", 
             (unsigned long long)expected, (unsigned long long)stx->stx_mnt_id);
    }
    printf("stx_mnt_id verified: %llu\n", (unsigned long long)stx->stx_mnt_id);
}

int main() {
    struct statx stx = {0};

    printf("--- Testing /etc/passwd ---\n");

    if (statx(AT_FDCWD, "/etc/passwd", 0, STATX_BASIC_STATS | STATX_MNT_ID | STATX_BTIME, &stx) != 0) {
        err(EXIT_FAILURE, "statx failed");
    }

    if (check_stx_mask(&stx, "STATX_BTIME", STATX_BTIME) == false) {
        err(EXIT_FAILURE, "requested btime but was not provided");
    }

    check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT);

    if (check_stx_mask(&stx, "STATX_MNT_ID", STATX_MNT_ID) == false) {
        err(EXIT_FAILURE, "requested mnt id but was not provided");
    }

    verify_mnt_id(&stx);

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == true) {
        errx(EXIT_FAILURE, "did not expect /etc/passwd to be amount point");
    }

    printf("--- Testing / ---\n");

    if (statx(AT_FDCWD, "/", 0, STATX_MNT_ID, &stx) != 0) {
        err(EXIT_FAILURE, "statx failed");
    }

    verify_mnt_id(&stx);

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == false) {
        errx(EXIT_FAILURE, "expected / to be a mount point");
    }

    printf("--- Testing /mnt/c ---\n");

    if (access("/mnt/c", F_OK) == 0) {
        if (statx(AT_FDCWD, "/mnt/c", 0, STATX_BASIC_STATS | STATX_MNT_ID, &stx) != 0) {
            err(EXIT_FAILURE, "statx failed");
        }

        verify_mnt_id(&stx);

        if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == false) {
            errx(EXIT_FAILURE, "expected /mnt/c to be a mount point");
        }
    } else {
        printf("skipping /mnt/c test, directory not found\n");
    }

    printf("--- Testing /etc ---\n");

    if (statx(AT_FDCWD, "/etc", 0, STATX_MNT_ID, &stx) != 0) {
        err(EXIT_FAILURE, "statx failed");
    }

    verify_mnt_id(&stx);

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == true) {
        errx(EXIT_FAILURE, "did not expect /etc to be a mount point");
    }

    return 0;
}
