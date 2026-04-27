#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/file.h>

static int (*sym_fcntl64)(int fd, int cmd, ...);
static int (*sym_fcntl)(int fd, int cmd, ...);

static void __attribute__((constructor)) init(void)
{
    sym_fcntl64 = dlsym(RTLD_NEXT, "fcntl64");
    sym_fcntl = dlsym(RTLD_NEXT, "fcntl");
    return;
}

static int fcntl_common(int (*real_fcntl)(int, int, ...), int fd, int cmd, void *arg, bool is64)
{
    if (cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW) {
        short l_type, l_whence;
        off64_t l_start, l_len;

        if (is64) {
            struct flock64 *fl = arg;
            l_type = fl->l_type;
            l_whence = fl->l_whence;
            l_start = fl->l_start;
            l_len = fl->l_len;
        } else {
            struct flock *fl = arg;
            l_type = fl->l_type;
            l_whence = fl->l_whence;
            l_start = fl->l_start;
            l_len = fl->l_len;
        }

        if (l_whence == SEEK_SET && l_start == 0 && l_len == 0) {
            int operation = 0;

            switch (l_type) {
                case F_RDLCK: operation = LOCK_SH; break;
                case F_WRLCK: operation = LOCK_EX; break;
                case F_UNLCK: operation = LOCK_UN; break;
            }

            if (operation != 0 || l_type == F_UNLCK) {
                if (cmd == F_OFD_SETLK && l_type != F_UNLCK) {
                    operation |= LOCK_NB;
                }
                return flock(fd, operation);
            }
        }
    }

    switch (cmd) {
        case F_OFD_SETLK:   cmd = F_SETLK;
                            break;
        case F_OFD_SETLKW:  cmd = F_SETLKW;
                            break;
        case F_OFD_GETLK:   cmd = F_GETLK;
                            break;
    }

    return real_fcntl(fd, cmd, arg);
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    return fcntl_common(sym_fcntl, fd, cmd, arg, false);
}

int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    return fcntl_common(sym_fcntl64, fd, cmd, arg, true);
}
