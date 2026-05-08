#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <errno.h>
#include <dlfcn.h>

static int (*sym_renameat2)(int olddirfd, const char *oldpath,
                            int newdirfd, const char *newpath, unsigned int flags);

static void __attribute__((constructor)) init(void)
{
    sym_renameat2 = dlsym(RTLD_NEXT, "renameat2");
}

int renameat2(int olddirfd, const char *oldpath,
              int newdirfd, const char *newpath, unsigned int flags)
{
    if (flags == RENAME_NOREPLACE) {
        if (linkat(olddirfd, oldpath, newdirfd, newpath, 0) != 0) {
            return -1;
        }
        return unlinkat(olddirfd, oldpath, 0);
    }

    if (flags == 0) {
        return renameat(olddirfd, oldpath, newdirfd, newpath);
    }

    return sym_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}
