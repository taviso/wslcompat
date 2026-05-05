#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

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
        void *probe = (void *) syscall(SYS_mmap, addr, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (probe == MAP_FAILED) {
            return MAP_FAILED;
        }

        // The kernel returned a different address, so was likely occupied.
        if (probe != addr) {
            syscall(SYS_munmap, probe, length);
            errno = EEXIST;
            return MAP_FAILED;
        }

        // Now try the real mapping with MAP_FIXED.
        flags &= ~MAP_FIXED_NOREPLACE;
        flags |=  MAP_FIXED;

        void *result = (void *) syscall(SYS_mmap, addr, length, prot, flags, fd, offset);

        // Clean up if that fails.
        if (result == MAP_FAILED) {
            syscall(SYS_munmap, probe, length);
        }

        return result;
    }

    return (void *) syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return mmap_common(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)
{
    return mmap_common(addr, length, prot, flags, fd, offset);
}
