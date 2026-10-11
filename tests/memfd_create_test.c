#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <err.h>

int main(int argc, char **argv)
{
    int fd;

    // Test 1: Basic memfd_create
    fd = memfd_create("test_memfd", 0);
    if (fd == -1) {
        err(EXIT_FAILURE, "memfd_create failed");
    }

    // Test 2: Check if we can write to it
    if (write(fd, "test", 4) != 4) {
        err(EXIT_FAILURE, "write to memfd failed");
    }

    // Test 3: Check if we can ftruncate it
    if (ftruncate(fd, 1024) != 0) {
        err(EXIT_FAILURE, "ftruncate memfd failed");
    }

    close(fd);

    printf("all tests pass\n");

    return 0;
}
