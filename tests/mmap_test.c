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
#include <valgrind/valgrind.h>

#include "testutil.h"

int main(int argc, char **argv)
{
    void *ptr;

    ptr = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(ptr != MAP_FAILED, "simple mmap");

    CHECK(mmap(ptr,
               PAGE_SIZE,
               PROT_NONE,
               MAP_FIXED | MAP_ANON | MAP_PRIVATE,
               -1,
               0) == ptr, "mmap_fixed");

    CHECK(mmap((void*)((uintptr_t)ptr + 1),
               PAGE_SIZE,
               PROT_NONE,
               MAP_FIXED | MAP_ANON | MAP_PRIVATE,
               -1,
               0) == MAP_FAILED, "mmap_fixed with unaligned addr fails");

    CHECK(errno == EINVAL, "mmap_fixed with unaligned addr sets EINVAL (got %d)", errno);

    CHECK(mmap(ptr,
               PAGE_SIZE,
               PROT_NONE,
               MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE,
               -1,
               0) == MAP_FAILED, "noreplace on existing mapping fails");

    CHECK(errno == EEXIST, "noreplace on existing mapping sets EEXIST (got %d)", errno);

    munmap(ptr, PAGE_SIZE);

    if (!RUNNING_ON_VALGRIND) {
        CHECK(mmap(ptr,
                   PAGE_SIZE,
                   PROT_NONE,
                   MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE,
                   -1,
                   0) == ptr, "noreplace on unmapped address succeeds");

        CHECK(mmap(NULL,
                   PAGE_SIZE,
                   PROT_NONE,
                   MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE,
                   -1,
                   0) == MAP_FAILED, "noreplace with NULL addr fails");

        CHECK(errno == EPERM, "noreplace with NULL addr sets EPERM (got %d)", errno);

        CHECK(mmap((void*)((uintptr_t)ptr + 1),
                   PAGE_SIZE,
                   PROT_NONE,
                   MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE,
                   -1,
                   0) == MAP_FAILED, "noreplace with unaligned addr fails");

        CHECK(errno == EINVAL, "noreplace with unaligned addr sets EINVAL (got %d)", errno);
    }

    void *garbage = mmap(NULL,
                         PAGE_SIZE,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_SHARED | MAP_ANON,
                         -1,
                         0);
                         
    CHECK(garbage == MAP_FAILED, "invalid flags fail");
    CHECK(errno == EINVAL, "invalid flags set EINVAL (got %d)", errno);

    void *all_flags = mmap(NULL,
                           PAGE_SIZE,
                           PROT_NONE,
                           ~0,
                           -1,
                           0);
                           
    CHECK(all_flags == MAP_FAILED, "all flags (~0) fail");
    
    if (wslcompat_test_on_wsl()) {
        // On WSL1, the shim intercepts MAP_FIXED_NOREPLACE at NULL and sets EPERM.
        // Natively, the kernel sees unknown bits and sets EOPNOTSUPP.
        CHECK(errno == EPERM || errno == EOPNOTSUPP || RUNNING_ON_VALGRIND, "all flags set EPERM or EOPNOTSUPP on WSL (got %d)", errno);
    } else {
        // On real Linux (which only runs natively), MAP_PRIVATE|MAP_SHARED triggers EINVAL.
        CHECK(errno == EINVAL || RUNNING_ON_VALGRIND, "all flags set EINVAL on Linux (got %d)", errno);
    }

    TEST_REPORT();
}
