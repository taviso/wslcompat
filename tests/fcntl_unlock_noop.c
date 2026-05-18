#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>

int main(void)
{
    char path[] = "/tmp/fcntl_unlock_noop_XXXXXX";
    int fd = mkostemp(path, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "F_UNLCK on never-locked file");
    }

    close(fd);
    unlink(path);
    printf("PASS\n");
    return 0;
}
