#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <errno.h>
#include <dlfcn.h>

#include "shim.h"
#include "logging.h"
#include "tunables.h"

SHIM_INIT(renameat2);

int renameat2(int olddirfd, const char *oldpath,
              int newdirfd, const char *newpath, unsigned int flags)
{

    if (wslcompat_passthru_self())
        return sym_next(renameat2, olddirfd, oldpath, newdirfd, newpath, flags);

    if (flags == RENAME_NOREPLACE) {
        if (linkat(olddirfd, oldpath, newdirfd, newpath, 0) != 0) {
            return -1;
        }
        return unlinkat(olddirfd, oldpath, 0);
    }

    if (flags == 0) {
        return renameat(olddirfd, oldpath, newdirfd, newpath);
    }

    return sym_next(renameat2, olddirfd, oldpath, newdirfd, newpath, flags);
}
