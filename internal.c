#define _GNU_SOURCE
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "internal.h"
#include "logging.h"

#define MAX_INTERNAL_FDS 16

static _Atomic(atomic_int *) registry[MAX_INTERNAL_FDS];
static _Atomic size_t registry_count;

// These routines are used to move internal book-keeping fds when the calling
// application wants to use that fd number. For example, it's common for shell
// scripts to use hardcoded fd numbers, or shell utilities to use close_range().
//
// We simply move our internal fd out of the way so the application can use the
// fd number however it wants.
void wslcompat_register_internal_fd(atomic_int *slot)
{
    size_t i = atomic_fetch_add(&registry_count, 1);
    if (i >= MAX_INTERNAL_FDS) {
        wslerror("internal fd registry full (cap=%d), slot %p dropped",
                 MAX_INTERNAL_FDS, (void *)slot);
        return;
    }
    atomic_store_explicit(&registry[i], slot, memory_order_release);
}

void wslcompat_relocate_internal_fd(int targetfd)
{
    size_t n = atomic_load(&registry_count);

    if (n > MAX_INTERNAL_FDS)
        n = MAX_INTERNAL_FDS;

    for (size_t i = 0; i < n; i++) {
        int newfd;
        int expected = targetfd;
        atomic_int *slot = atomic_load_explicit(&registry[i], memory_order_acquire);

        if (!slot)
            continue;

        if (atomic_load(slot) != targetfd)
            continue;

        if ((newfd = syscall(SYS_fcntl, targetfd, F_DUPFD_CLOEXEC, 3)) < 0) {
            wslwarn("could not rescue internal fd=%d: %m", targetfd);
            atomic_store(slot, -2);
            continue;
        }

        // Atomic so two closers dont both succeed and leak a dup.
        if (!atomic_compare_exchange_strong(slot, &expected, newfd)) {
            syscall(SYS_close, newfd);
            continue;
        }

        wsldbg("moved internal fd %d -> %d", targetfd, newfd);
    }
}
