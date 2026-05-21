#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(mincore);

int mincore(void *addr, size_t length, unsigned char *vec)
{
    if (wslcompat_passthru("mincore"))
        return sym_next(mincore, addr, length, vec);

    if (msync(addr, length, MS_ASYNC) != 0) {
        wsllog("msync(%p, %lu, %p) failed, %m", addr, length, vec);
        return -1;
    }

    // We can't know for sure if it's in core, so we'll just say yes (1).
    memset(vec, 1, (length + PAGE_SIZE - 1) / PAGE_SIZE);

    return 0;
}
