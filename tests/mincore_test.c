#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <string.h>

#include "testutil.h"

int main(int argc, char **argv)
{
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned char vec[2];
    void *addr;

    // Test 1: Valid mapping
    // We use MAP_LOCKED to ensure it's actually in-core on real linux.
    addr = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    CHECK(addr != MAP_FAILED, "mmap 2 pages");

    // mincore should succeed on a valid mapping.
    CHECK(mincore(addr, page_size * 2, vec) == 0, "mincore valid mapping");
    CHECK(vec[0] == 1 && vec[1] == 1, "mincore returned 1 for resident pages");

    // Test 2: Unaligned addr
    errno = 0;
    int r = mincore(addr + 1, page_size, vec);
    CHECK(r == -1 && errno == EINVAL, "mincore unaligned addr returns EINVAL");

    // Test 3: Zero length
    errno = 0;
    r = mincore(addr, 0, vec);
    CHECK(r == 0, "mincore zero length succeeds");

    // Test 4: Partial mapping
    munmap(addr + page_size, page_size);
    errno = 0;
    r = mincore(addr, page_size * 2, vec);
    CHECK(r == -1 && errno == ENOMEM, "mincore partial unmapped region returns ENOMEM");

    // Test 5: Invalid mapping
    munmap(addr, page_size);
    errno = 0;
    r = mincore(addr, page_size, vec);
    CHECK(r == -1 && errno == ENOMEM, "mincore fully unmapped region returns ENOMEM");

    TEST_REPORT();
    return 0;
}
