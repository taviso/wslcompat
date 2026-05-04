#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct file_handle *fh;
    int mount_id;
    int fd;

    // Test 1: Basic name_to_handle_at on /etc/passwd
    fh = malloc(sizeof(struct file_handle));
    if (!fh) {
        err(EXIT_FAILURE, "malloc");
    }
    fh->handle_bytes = 0;

    if (name_to_handle_at(AT_FDCWD, "/etc/passwd", fh, &mount_id, 0) != -1) {
        errx(EXIT_FAILURE, "name_to_handle_at succeeded with 0 handle_bytes");
    }

    if (errno == ENOSYS) {
        errx(EXIT_FAILURE, "name_to_handle_at not implemented");
        return 0;
    }

    if (errno != EOVERFLOW) {
        err(EXIT_FAILURE, "name_to_handle_at failed with unexpected error");
    }

    fh = realloc(fh, sizeof(struct file_handle) + fh->handle_bytes);
    if (!fh) {
        err(EXIT_FAILURE, "realloc");
    }

    if (name_to_handle_at(AT_FDCWD, "/etc/passwd", fh, &mount_id, 0) != 0) {
        err(EXIT_FAILURE, "name_to_handle_at failed");
    }

    // Test 2: AT_EMPTY_PATH
    fd = open("/etc/passwd", O_RDONLY);
    if (fd == -1) {
        err(EXIT_FAILURE, "open /etc/passwd");
    }

    // Reuse handle structure, reset size
    fh->handle_bytes = 0;
    if (name_to_handle_at(fd, "", fh, &mount_id, AT_EMPTY_PATH) != -1) {
        errx(EXIT_FAILURE, "name_to_handle_at AT_EMPTY_PATH succeeded with 0 handle_bytes");
    }

    if (errno != EOVERFLOW) {
        err(EXIT_FAILURE, "name_to_handle_at AT_EMPTY_PATH failed with unexpected error");
    }

    fh = realloc(fh, sizeof(struct file_handle) + fh->handle_bytes);
    if (!fh) {
        err(EXIT_FAILURE, "realloc");
    }

    if (name_to_handle_at(fd, "", fh, &mount_id, AT_EMPTY_PATH) != 0) {
        err(EXIT_FAILURE, "name_to_handle_at AT_EMPTY_PATH failed");
    }

    close(fd);
    free(fh);

    printf("all tests pass\n");

    return 0;
}
