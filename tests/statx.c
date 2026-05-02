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

void check_stx_mask(struct statx *stx, const char *name, int flag)
{
    if (stx->stx_mask & flag)
        printf("stx_mask *did* provide %s\n", name);
    else
        printf("stx_mask *did not* provide %s\n", name);
    return;
}

void check_stx_attribute(struct statx *stx, const char *name, uint64_t flag)
{
    if (stx->stx_attributes_mask & flag) {
        if (stx->stx_attributes & flag)
            printf("stx_attributes *did* have %s set\n", name);
        else
            printf("stx_attributes *did not* have %s set\n", name);
    } else {
        printf("stx_attributes_mask *did not* include %s\n", name);
    }
}

int main() {
    struct statx stx = {0};
    int ret;

    printf("--- Testing /etc/passwd ---\n");
    ret = statx(AT_FDCWD, "/etc/passwd", 0, STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID, &stx);

    if (ret == -1) {
        err(EXIT_FAILURE, "statx failed");
    }

    printf("statx: success\n");

    check_stx_mask(&stx, "STATX_TYPE", STATX_TYPE);
    check_stx_mask(&stx, "STATX_MODE", STATX_MODE);
    check_stx_mask(&stx, "STATX_NLINK", STATX_NLINK);
    check_stx_mask(&stx, "STATX_UID", STATX_UID);
    check_stx_mask(&stx, "STATX_GID", STATX_GID);
    check_stx_mask(&stx, "STATX_ATIME", STATX_ATIME);
    check_stx_mask(&stx, "STATX_MTIME", STATX_MTIME);
    check_stx_mask(&stx, "STATX_CTIME", STATX_CTIME);
    check_stx_mask(&stx, "STATX_INO", STATX_INO);
    check_stx_mask(&stx, "STATX_SIZE", STATX_SIZE);
    check_stx_mask(&stx, "STATX_BLOCKS", STATX_BLOCKS);
    check_stx_mask(&stx, "STATX_BTIME", STATX_BTIME);
    check_stx_mask(&stx, "STATX_MNT_ID", STATX_MNT_ID);
    check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT);

    printf("\n--- Testing / ---\n");
    memset(&stx, 0, sizeof(stx));
    ret = statx(AT_FDCWD, "/", 0, STATX_BASIC_STATS | STATX_MNT_ID, &stx);
    if (ret == -1) {
        err(EXIT_FAILURE, "statx failed on /");
    }
    printf("statx: success\n");
    check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT);

    printf("\n--- Testing /mnt/c ---\n");
    memset(&stx, 0, sizeof(stx));
    ret = statx(AT_FDCWD, "/mnt/c", 0, STATX_BASIC_STATS | STATX_MNT_ID, &stx);
    if (ret == -1) {
        err(EXIT_FAILURE, "statx failed on /mnt/c");
    }
    printf("statx: success\n");
    check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT);

    printf("\n--- Testing /etc ---\n");
    memset(&stx, 0, sizeof(stx));
    ret = statx(AT_FDCWD, "/etc", 0, STATX_BASIC_STATS | STATX_MNT_ID, &stx);
    if (ret == -1) {
        err(EXIT_FAILURE, "statx failed on /etc");
    }
    printf("statx: success\n");
    check_stx_attribute(&stx, "STATX_ATTR_MOUNT_ROOT", STATX_ATTR_MOUNT_ROOT);

    return 0;
}
