#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>

static long (*sym_syscall)(long number, ...);

static int __attribute__((constructor)) init(void)
{
    if ((sym_syscall = dlsym(RTLD_NEXT, "syscall")) == NULL)
        return -1;
    return 0;
}

long syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);

    if (__builtin_expect(sym_syscall == NULL, false)) {
        // Initialization order error, call constructor
        if (init() != 0) return -1;
    }

    // We can intercept syscall numbers we want to polyfill here.
    // switch (number) { ... }

    long a0 = va_arg(ap, long);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);

    va_end(ap);

    return sym_syscall(number, a0, a1, a2, a3, a4, a5);
}
