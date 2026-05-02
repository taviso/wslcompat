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

int main() {
    struct statx stx = {0};

    printf("--- Testing /etc/passwd ---\n");

    if (statx(AT_FDCWD, "/etc/passwd", 0, STATX_BASIC_STATS | STATX_MNT_ID, &stx) != 0) {
        errx(EXIT_FAILURE, "statx failed");
    }

    //check_stx_mask(&stx, "STATX_TYPE", STATX_TYPE);
    //check_stx_mask(&stx, "STATX_MODE", STATX_MODE);
    //check_stx_mask(&stx, "STATX_NLINK", STATX_NLINK);
    //check_stx_mask(&stx, "STATX_UID", STATX_UID);
    //check_stx_mask(&stx, "STATX_GID", STATX_GID);
    //check_stx_mask(&stx, "STATX_ATIME", STATX_ATIME);
    //check_stx_mask(&stx, "STATX_MTIME", STATX_MTIME);
    //check_stx_mask(&stx, "STATX_CTIME", STATX_CTIME);
    //check_stx_mask(&stx, "STATX_INO", STATX_INO);
    //check_stx_mask(&stx, "STATX_SIZE", STATX_SIZE);
    //check_stx_mask(&stx, "STATX_BLOCKS", STATX_BLOCKS);
    //check_stx_mask(&stx, "STATX_BTIME", STATX_BTIME);

    check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT);

    if (check_stx_mask(&stx, "STATX_MNT_ID", STATX_MNT_ID) == false) {
        err(EXIT_FAILURE, "requested mnt id but was not provided");
    }

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == true) {
        errx(EXIT_FAILURE, "did not expect /etc/passwd to be amount point");
    }

    printf("--- Testing / ---\n");

    if (statx(AT_FDCWD, "/", 0, STATX_MNT_ID, &stx) != 0) {
        errx(EXIT_FAILURE, "statx failed");
    }

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == false) {
        errx(EXIT_FAILURE, "expected / to be a mount point");
    }

    printf("--- Testing /mnt/c ---\n");

    if (statx(AT_FDCWD, "/mnt/c", 0, STATX_BASIC_STATS | STATX_MNT_ID, &stx) != 0) {
        errx(EXIT_FAILURE, "statx failed");
    }

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == false) {
        errx(EXIT_FAILURE, "expected /mnt/c to be a mount point");
    }

    printf("--- Testing /etc ---\n");

    if (statx(AT_FDCWD, "/etc", 0, STATX_MNT_ID, &stx) != 0) {
        errx(EXIT_FAILURE, "statx failed");
    }

    if (check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT) == true) {
        errx(EXIT_FAILURE, "did not expect /etc to be a mount point");
    }

    return 0;
}
