#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

static void * (*sym_mmap)(void *, size_t, int, int, int, off_t);

static void __attribute__((constructor)) init(void)
{
    sym_mmap = dlsym(RTLD_NEXT, "mmap");
    return;
}

static bool check_page_mapped(uintptr_t ptr) {
    FILE *f;
    uintptr_t start;
    uintptr_t end;
    bool result = false;

    f = fopen("/proc/self/maps", "r");

    while (fscanf(f, "%lx-%lx %*s %*x %*x:%*x %*d %*s\n", &start, &end) == 2) {
        if (ptr >= start && ptr < end) {
            result = true;
            break;
        }
    }

    fclose(f);
    return result;
}

static bool check_range_mapped(uintptr_t ptr, size_t length) {
    size_t pagecnt = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < pagecnt; i++) {
        if (check_page_mapped(ptr + i * PAGE_SIZE))
            return true;
    }
    return false;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    uint8_t vec[1024] = {0};

    // Handle any flags that WSL1 rejects
    if (flags & MAP_FIXED_NOREPLACE) {
        // So we can emulate this by returning EEXIST if the mapping already
        // exists.
        flags &= ~MAP_FIXED_NOREPLACE;
        flags |=  MAP_FIXED;

        if (check_range_mapped((uintptr_t)(addr), length)) {
            // This should fail with EEXIST
            errno = EEXIST;

            // Return failure
            return MAP_FAILED;
        }
    }

    return sym_mmap(addr, length, prot, flags, fd, offset);
}
