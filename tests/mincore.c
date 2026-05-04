#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv)
{
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned char vec[1];
    void *addr;

    // Test 1: Valid mapping
    addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        err(EXIT_FAILURE, "mmap");
    }

    // mincore should succeed on a valid mapping.
    if (mincore(addr, page_size, vec) != 0) {
        err(EXIT_FAILURE, "mincore valid mapping failed");
    }

    if (vec[0] != 1) {
        errx(EXIT_FAILURE, "mincore returned incorrect residency (expected 1)");
    }

    if (munmap(addr, page_size) != 0) {
        err(EXIT_FAILURE, "munmap");
    }

    // Test 2: Invalid mapping
    // mincore should fail with ENOMEM for unmapped regions.
    if (mincore(addr, page_size, vec) == 0) {
        errx(EXIT_FAILURE, "mincore on unmapped region succeeded unexpectedly");
    }

    if (errno != ENOMEM) {
        err(EXIT_FAILURE, "mincore on unmapped region returned wrong errno");
    }

    printf("all tests pass\n");

    return 0;
}
