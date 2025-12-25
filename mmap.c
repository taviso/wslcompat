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
static void * (*sym_mmap64)(void *, size_t, int, int, int, off64_t);

static FILE *maps;

static void __attribute__((constructor)) init(void)
{
    sym_mmap = dlsym(RTLD_NEXT, "mmap");
    sym_mmap64 = dlsym(RTLD_NEXT, "mmap64");
    return;
}

static void __attribute__((destructor)) fini(void)
{
    if (maps) fclose(maps);
    return;
}

static bool check_range_mapped(uintptr_t ptr, size_t len) {
    uintptr_t start;
    uintptr_t end;
    bool result = true;

    if (maps == NULL) {
        maps = fopen("/proc/self/maps", "r");
    }

    rewind(maps);

    while (fscanf(maps, "%lx-%lx %*s %*x %*x:%*x %*d %*s\n", &start, &end) == 2) {
        if (start >= ptr && ptr + len < end) {
            result = false;
            break;
        }
    }

    return result;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
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

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)
{
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

    return sym_mmap64(addr, length, prot, flags, fd, offset);
}
