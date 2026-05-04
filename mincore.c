#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

static int (*sym_mincore)(void *, size_t, unsigned char *);

static void __attribute__((constructor)) init(void)
{
    sym_mincore = dlsym(RTLD_NEXT, "mincore");
}

int mincore(void *addr, size_t length, unsigned char *vec)
{
    if (msync(addr, length, MS_ASYNC) != 0) {
        return -1;
    }

    // We can't know for sure if it's in core, so we'll just say yes (1).
    memset(vec, 1, (length + PAGE_SIZE - 1) / PAGE_SIZE);
    return 0;
}
