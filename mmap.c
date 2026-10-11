#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "logging.h"
#include "shim.h"
#include "synth.h"
#include "tunables.h"
#include "utils.h"

SHIM_INIT(mmap);
SHIM_ALIAS(mmap);

// Help to handle any synthetic fds passed to mmap.
static void *mmap_synth_fallback(void *addr, size_t length, int prot, int flags, int *fd, off64_t offset)
{
    SYNTH_AUTOPIN_FD(synth, *fd);

    if (synth == NULL)
        return errno = EBADF, MAP_FAILED;

    if (synth->ops->mmap) {
        return synth->ops->mmap(synth, addr, length, prot, flags, *fd, offset);
    }

    if (synth->ops->passthru) {
        if ((*fd = synth->ops->passthru(synth, SYNTH_OP_IO)) < 0) {
            return errno = ENODEV, MAP_FAILED;
        }
        return sym_next(mmap, addr, length, prot, flags, *fd, offset);
    }

    wsltrace("fd %d is a synthetic fd, but no handler", *fd);

    return errno = ENODEV, MAP_FAILED;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *result;
    int origflags = flags;
    void *probe = MAP_FAILED;

    result = sym_next(mmap, addr, length, prot, flags, fd, offset);

    if (result != MAP_FAILED || wslcompat_passthru_self())
        return result;

    wsltrace("%p, %ld, %#x, %#x, %d, %ld = %m", addr, length, prot, flags, fd, offset);

    // Check for a synthetic file descriptor
    if (errno == EBADF) {
        result = mmap_synth_fallback(addr, length, prot, flags, &fd, offset);
        if (result != MAP_FAILED)
            return result;
    }

    if (errno != EINVAL && errno != EOPNOTSUPP)
        return result;

    // Check if there were flags we can emulate.
    if ((flags & (MAP_FIXED_NOREPLACE | MAP_LOCKED)) == 0)
        return result;

    // Strip flags known to be unsupported by the WSL1 kernel.
    flags &= ~MAP_FIXED_NOREPLACE;
    flags &= ~MAP_LOCKED;
    // Add future flags here.

    // This flag is not implemented by WSL1, we need to emulate it.
    if (origflags & MAP_FIXED_NOREPLACE) {
        // Only root can (usually) mmap at NULL
        if (addr == NULL) {
            return errno = EPERM, MAP_FAILED;
        }

        // Addresses must be page aligned.
        if ((uintptr_t)addr & ~PAGE_MASK) {
            return errno = EINVAL, MAP_FAILED;
        }

        // Attempt to reserve the range to see if the kernel gives it to us.
        probe = (void *) syscall(SYS_mmap, addr, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        // Nothing much we can do, give up.
        if (probe == MAP_FAILED)
            return MAP_FAILED;

        // The kernel returned a different address, so was likely occupied.
        if (probe != addr) {
            syscall(SYS_munmap, probe, length);
            return errno = EEXIST, MAP_FAILED;
        }

        // Now we can map with MAP_FIXED, and safely replace.
        flags |= MAP_FIXED;
    }

    result = sym_next(mmap, addr, length, prot, flags, fd, offset);

    // Clean up any probes if that fails.
    if (result == MAP_FAILED && probe != MAP_FAILED) {
        preserve_errno();
        syscall(SYS_munmap, probe, length);
        return MAP_FAILED;
    }

    // Handle lock emulation.
    if (result != MAP_FAILED && (origflags & MAP_LOCKED)) {
        if (syscall(SYS_mlock, result, length) != 0) {
            syscall(SYS_munmap, result, length);
            return errno = ENOMEM, MAP_FAILED;
        }
    }

    return result;
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset) __attribute__((alias("mmap")));
