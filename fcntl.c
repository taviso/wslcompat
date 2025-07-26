#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>

static int (*sym_fcntl64)(int fd, int cmd, ...);
static int (*sym_fcntl)(int fd, int cmd, ...);

static void __attribute__((constructor)) init(void)
{
    sym_fcntl64 = dlsym(RTLD_NEXT, "fcntl64");
    sym_fcntl = dlsym(RTLD_NEXT, "fcntl");
    return;
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    switch (cmd) {
        case F_OFD_SETLK:   cmd = F_SETLK;
                            break;
        case F_OFD_SETLKW:  cmd = F_SETLKW;
                            break;
        case F_OFD_GETLK:   cmd = F_GETLK;
                            break;
    }

    return sym_fcntl(fd, cmd, arg);
}

int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    switch (cmd) {
        case F_OFD_SETLK:   cmd = F_SETLK;
                            break;
        case F_OFD_SETLKW:  cmd = F_SETLKW;
                            break;
        case F_OFD_GETLK:   cmd = F_GETLK;
                            break;
    }

    return sym_fcntl64(fd, cmd, arg);
}
