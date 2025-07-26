#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>

int main(int argc, char **argv)
{
    void *ptr, *tmp;

    ptr = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (ptr == MAP_FAILED) {
        err(EXIT_FAILURE, "simple mmap failed");
    }

    if (mmap(ptr,
             PAGE_SIZE,
             PROT_NONE,
             MAP_FIXED | MAP_ANON | MAP_PRIVATE,
             -1,
             0) != ptr) {
        err(EXIT_FAILURE, "mmap_fixed failed");
    }

    if (mmap(ptr,
             PAGE_SIZE,
             PROT_NONE,
             MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE,
             -1,
             0) != MAP_FAILED) {
        err(EXIT_FAILURE, "noreplace ignored");
    }

    if (errno != EEXIST) {
        err(EXIT_FAILURE, "noreplace failed, but wrong errno");
    }

    munmap(ptr, PAGE_SIZE);

    if (mmap(ptr,
             PAGE_SIZE,
             PROT_NONE,
             MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE,
             -1,
             0) != ptr) {
        err(EXIT_FAILURE, "noreplace failed");
    }

    printf("all tests pass\n");

    return 0;
}
