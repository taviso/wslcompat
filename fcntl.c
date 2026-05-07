#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/unistd.h>
#include <sys/syscall.h>

static int (*sym_fcntl)(int fd, int cmd, ...);

static void __attribute__((constructor)) init(void)
{
    sym_fcntl = dlsym(RTLD_NEXT, "fcntl");
}

static inline int fcntl_translate_type(int type)
{
    switch (type) {
        case F_RDLCK: return LOCK_SH;
        case F_WRLCK: return LOCK_EX;
        case F_UNLCK: return LOCK_UN;
    }
    return -1;
}

static int flock_status_query(int fd, struct flock *fl)
{
    int result = -1;
    int test_fd;
    char path[64];

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

    if ((test_fd = syscall(SYS_open, path, O_RDONLY)) < 0)
        return -1;

    if (flock(test_fd, LOCK_EX | LOCK_NB) == 0) {
        fl->l_type = F_UNLCK;
        fl->l_whence = SEEK_SET;
        fl->l_start = 0;
        fl->l_len = 0;
        fl->l_pid = -1;
        result = 0;
        goto cleanup;
    }

    if (errno != EWOULDBLOCK)
        goto cleanup;

    if (flock(test_fd, LOCK_SH | LOCK_NB) == 0) {
        fl->l_type = F_RDLCK;
        fl->l_whence = SEEK_SET;
        fl->l_start = 0;
        fl->l_len = 0;
        fl->l_pid = -1;
        result = 0;
        goto cleanup;
    }

    if (errno != EWOULDBLOCK)
        goto cleanup;

    fl->l_type = F_WRLCK;
    fl->l_whence = SEEK_SET;
    fl->l_start = 0;
    fl->l_len = 0;
    fl->l_pid = -1;
    result = 0;

  cleanup:
    syscall(SYS_close, test_fd);
    return result;
}

static int fcntl_lock_shim(int fd, int cmd, struct flock *fl)
{
    int operation;
    struct flock hybrid = {
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_type = fl->l_type,
        .l_len = 0,
    };

    if (cmd == F_GETLK || cmd == F_OFD_GETLK) {
        // Try the native POSIX check first.
        sym_fcntl(fd, F_GETLK, fl);

        if (fl->l_type != F_UNLCK) {
            if (cmd == F_OFD_GETLK) {
                fl->l_pid = -1;
            }
            return 0;
        }

        // Fall back to the /proc trick for flock() visibility.
        return flock_status_query(fd, fl);
    }

    // Translate this POSIX operation into a flock operation.
    if ((operation = fcntl_translate_type(fl->l_type)) < 0)
        return -1;

    // Mirror as POSIX for metadata and "close drops locks" behavior.
    // We always use F_SETLK (non-blocking); the real block happens in flock().
    sym_fcntl(fd, F_SETLK, &hybrid);

    // Apply the real enforcement via flock().
    if (cmd == F_SETLK || cmd == F_OFD_SETLK) {
        if (fl->l_type != F_UNLCK) {
            operation |= LOCK_NB;
        }
    }

    return flock(fd, operation);
}

static int fcntl_common(int fd, int cmd, void *arg)
{
    switch (cmd) {
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
        case F_OFD_GETLK:
        case F_OFD_SETLK:
        case F_OFD_SETLKW:
            return fcntl_lock_shim(fd, cmd, arg);
    }
    return sym_fcntl(fd, cmd, arg);
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    return fcntl_common(fd, cmd, arg);
}

int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    return fcntl_common(fd, cmd, arg);
}
