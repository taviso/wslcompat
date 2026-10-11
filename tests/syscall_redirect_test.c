#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <valgrind/valgrind.h>

#include "testutil.h"

int main(int argc, char **argv)
{
    long page = sysconf(_SC_PAGESIZE);
    char buf[64];
    ssize_t n;
    void *addr;
    int lockfd;

    if (RUNNING_ON_VALGRIND) {
        printf("SKIP: running under valgrind, cannot set xattr on self\n");
        return 0;
    }

    lockfd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (lockfd < 0)
        err(EXIT_FAILURE, "open /proc/self/exe");
    if (flock(lockfd, LOCK_EX) < 0)
        err(EXIT_FAILURE, "flock");

    n = fgetxattr(lockfd, "user.wslcompat.syscall", buf, sizeof(buf));
    if (n < 0) {
        if (errno != ENODATA)
            err(EXIT_FAILURE, "fgetxattr");

        if (fsetxattr(lockfd, "user.wslcompat.syscall",
                     "mmap", strlen("mmap"), 0) < 0)
            err(EXIT_FAILURE, "fsetxattr");

        execl("/proc/self/exe", argv[0], NULL);
        err(EXIT_FAILURE, "execl");
    }

    addr = mmap(NULL, page, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (addr == MAP_FAILED)
        err(EXIT_FAILURE, "initial mmap failed");

    munmap(addr, page);

    void *result = (void *)syscall(SYS_mmap,
                                   (long)addr,
                                   (long)page,
                                   (long)PROT_NONE,
                                   (long)(MAP_FIXED_NOREPLACE | MAP_ANON | MAP_PRIVATE),
                                   (long)-1,
                                   (long)0);

    if (result == MAP_FAILED)
        err(EXIT_FAILURE, "syscall(SYS_mmap) MAP_FIXED_NOREPLACE");

    if (result != addr)
        errx(EXIT_FAILURE, "returned %p, expected %p", result, addr);

    munmap(addr, page);
    fremovexattr(lockfd, "user.wslcompat.syscall");
    printf("PASS: syscall(SYS_mmap) redirected through mmap polyfill\n");
    return 0;
}
