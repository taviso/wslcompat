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
static int (*sym_munmap)(void *, size_t);

static void __attribute__((constructor)) init(void)
{
    sym_mmap = dlsym(RTLD_NEXT, "mmap");
    sym_mmap64 = dlsym(RTLD_NEXT, "mmap64");
    sym_munmap = dlsym(RTLD_NEXT, "munmap");
}

static void *mmap_common(void *addr,
                         size_t length,
                         int prot,
                         int flags,
                         int fd,
                         off64_t offset)
{
    // This flag is not implemented by WSL1, we need to emulate it.
    if (flags & MAP_FIXED_NOREPLACE) {
        // Attempt to reserve the range to see if the kernel gives it to us.
        void *probe = sym_mmap64(addr, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (probe == MAP_FAILED) {
            return MAP_FAILED;
        }

        // The kernel returned a different address, so was likely occupied.
        if (probe != addr) {
            sym_munmap(probe, length);
            errno = EEXIST;
            return MAP_FAILED;
        }

        // Now try the real mapping with MAP_FIXED.
        flags &= ~MAP_FIXED_NOREPLACE;
        flags |=  MAP_FIXED;

        void *result = sym_mmap64(addr, length, prot, flags, fd, offset);

        // Clean up if that fails.
        if (result == MAP_FAILED) {
            sym_munmap(probe, length);
        }

        return result;
    }

    return sym_mmap64(addr, length, prot, flags, fd, offset);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return mmap_common(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)
{
    return mmap_common(addr, length, prot, flags, fd, offset);
}
