#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "logging.h"
#include "utils.h"

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

void wslcompat_walk_proc_self_fd(wslcompat_fd_visit_fn visit, void *user)
{
    char buf[4096];
    long n;
    int dirfd;

    if ((dirfd = syscall(SYS_open,
                         "/proc/self/fd",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
        wslwarn("could not open /proc/self/fd: %m");
        return;
    }

    while ((n = syscall(SYS_getdents64, dirfd, buf, sizeof(buf))) > 0) {
        for (long off = 0; off < n;) {
            struct linux_dirent64 *ent = (void *)(buf + off);
            off += ent->d_reclen;

            if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
                continue;

            int fd = atoi(ent->d_name);
            if (fd == dirfd)
                continue;

            if (visit(fd, user) != 0)
                goto done;
        }
    }
done:
    syscall(SYS_close, dirfd);
}
