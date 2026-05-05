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
    void *result;
    void *probe;
    int origflags = flags;

    // Strip flags known to be unsupported by the WSL1 kernel.
    flags &= ~MAP_LOCKED;
    flags &= ~MAP_FIXED_NOREPLACE;

    // Some flags require a probe mapping.
    probe = MAP_FAILED;

    // This flag is not implemented by WSL1, we need to emulate it.
    if (origflags & MAP_FIXED_NOREPLACE) {
        // Attempt to reserve the range to see if the kernel gives it to us.
        probe = (void *) syscall(SYS_mmap, addr, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        // Nothing much we can do, give up.
        if (probe == MAP_FAILED) {
            return MAP_FAILED;
        }

        // The kernel returned a different address, so was likely occupied.
        if (probe != addr) {
            syscall(SYS_munmap, probe, length);
            errno = EEXIST;
            return MAP_FAILED;
        }

        // Now we can map with MAP_FIXED, and safely replace.
        flags |= MAP_FIXED;
    }

    result = (void *) syscall(SYS_mmap, addr, length, prot, flags, fd, offset);

    // Clean up any probes if that fails.
    if (result == MAP_FAILED && probe != MAP_FAILED) {
        syscall(SYS_munmap, probe, length);
    }

    // Handle lock emulation.
    if (result != MAP_FAILED && (origflags & MAP_LOCKED)) {
        if (syscall(SYS_mlock, result, length) != 0) {
            syscall(SYS_munmap, result, length);
            errno = ENOMEM;
            return MAP_FAILED;
        }
    }

    return result;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return mmap_common(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)
{
    return mmap_common(addr, length, prot, flags, fd, offset);
}
