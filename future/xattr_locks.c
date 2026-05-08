#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/syscall.h>

#define MAX_SLOTS 8

static int (*sym_fcntl)(int fd, int cmd, ...);

static void __attribute__((constructor)) init(void)
{
    sym_fcntl = dlsym(RTLD_NEXT, "fcntl");
}

static unsigned long get_start_time(pid_t pid)
{
    char buf[512], path[64], *p;
    int fd;
    ssize_t n;
    int count = 0;
    unsigned long start_time = 0;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fd = open(path, O_RDONLY);

    if (fd < 0) {
        return 0;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return 0;
    }

    buf[n] = '\0';

    p = strrchr(buf, ')');

    if (p == NULL) {
        return 0;
    }

    while (*p != '\0' && count < 19) {
        if (*p == ' ') {
            count = count + 1;
        }
        p = p + 1;
    }

    if (sscanf(p, "%lu", &start_time) != 1) {
        return 0;
    }

    return start_time;
}

static int range_conflict(struct flock *a, struct flock *b)
{
    // Readers don't conflict with other readers
    if (a->l_type == F_RDLCK && b->l_type == F_RDLCK) {
        return 0;
    }

    // A whole-file lock (len=0) conflicts with any other lock
    if (a->l_len == 0 || b->l_len == 0) {
        return 1;
    }

    // Check if range A is completely after range B
    if (a->l_start >= b->l_start + b->l_len) {
        return 0;
    }

    // Check if range B is completely after range A
    if (b->l_start >= a->l_start + a->l_len) {
        return 0;
    }

    // Ranges must overlap
    return 1;
}

static int scan_range_conflicts(int fd, struct flock *fl)
{
    char attr[32], val[128];
    struct flock other;
    pid_t owner;
    unsigned long start_time;
    unsigned long my_start = get_start_time(getpid());
    int i;

    for (i = 0; i < MAX_SLOTS; i++) {
        snprintf(attr, sizeof(attr), "user.wslcompat.lock.%d", i);

        if (fgetxattr(fd, attr, val, sizeof(val)) <= 0) {
            continue;
        }

        if (sscanf(val, "%d:%lu:%hd:%ld:%ld", &owner, &start_time, &other.l_type, &other.l_start, &other.l_len) != 5) {
            fremovexattr(fd, attr);
            continue;
        }

        // We don't conflict with our own locks
        if (owner == getpid() && start_time == my_start) {
            continue;
        }

        // Check if the owner process is still alive
        if (kill(owner, 0) != 0 && errno == ESRCH) {
            fremovexattr(fd, attr);
            continue;
        }

        // Verify PID hasn't been reused
        if (get_start_time(owner) != start_time) {
            fremovexattr(fd, attr);
            continue;
        }

        // Check if the active lock overlaps with our requested range
        if (range_conflict(fl, &other)) {
            return 1;
        }
    }
    return 0;
}

static int fcntl_lock_shim(int fd, int cmd, struct flock *fl)
{
    char attr[32], val[128];
    int operation;
    int i;
    unsigned long my_start = get_start_time(getpid());

    // --- CASE 1: Whole-File Lock ---
    // We treat 0-0 as a traditional whole-file lock using flock().
    if (fl->l_start == 0 && fl->l_len == 0) {
        if (fl->l_type == F_RDLCK) {
            operation = LOCK_SH;
        } else if (fl->l_type == F_WRLCK) {
            operation = LOCK_EX;
        } else {
            operation = LOCK_UN;
        }

        if (cmd == F_SETLK || cmd == F_OFD_SETLK) {
            operation = operation | LOCK_NB;
        }

        // Place the primary lock.
        if (flock(fd, operation) < 0) {
            return -1;
        }

        if (operation == LOCK_UN) {
            return 0;
        }

        // If we acquired the file lock, we must still check if any 
        // byte-range locks exist that would block a whole-file lock.
        if (scan_range_conflicts(fd, fl)) {
            flock(fd, LOCK_UN);
            errno = EAGAIN;
            return -1;
        }

        return 0;
    }

    // --- CASE 2: Range Lock ---
    // We synchronize the xattr "bulletin board" using a short-term flock.
    if (flock(fd, LOCK_EX) < 0) {
        return -1;
    }

    if (fl->l_type == F_UNLCK) {
        for (i = 0; i < MAX_SLOTS; i++) {
            pid_t owner;
            unsigned long start;
            snprintf(attr, sizeof(attr), "user.wslcompat.lock.%d", i);

            if (fgetxattr(fd, attr, val, sizeof(val)) <= 0) {
                continue;
            }

            if (sscanf(val, "%d:%lu:", &owner, &start) != 2) {
                continue;
            }

            if (owner == getpid() && start == my_start) {
                fremovexattr(fd, attr);
            }
        }
        flock(fd, LOCK_UN);
        return 0;
    }

    // Before placing a range lock, check if it conflicts with existing ranges.
    // (A whole-file flock(LOCK_EX) by another process would have blocked us 
    // at the top of this section).
    if (scan_range_conflicts(fd, fl)) {
        flock(fd, LOCK_UN);
        errno = EAGAIN;
        return -1;
    }

    // Find an empty slot and post our range.
    for (i = 0; i < MAX_SLOTS; i++) {
        snprintf(attr, sizeof(attr), "user.wslcompat.lock.%d", i);

        if (fgetxattr(fd, attr, val, sizeof(val)) > 0) {
            continue;
        }

        snprintf(val, sizeof(val), "%d:%lu:%hd:%ld:%ld", getpid(), my_start, fl->l_type, fl->l_start, fl->l_len);

        if (fsetxattr(fd, attr, val, strlen(val), 0) < 0) {
            flock(fd, LOCK_UN);
            return -1;
        }

        flock(fd, LOCK_UN);
        return 0;
    }

    flock(fd, LOCK_UN);
    errno = ENOLCK;
    return -1;
}

static int fcntl_common(int fd, int cmd, void *arg)
{
    int result;
    struct flock *fl = arg;

    switch (cmd) {
        case F_SETLK:
        case F_OFD_SETLK:
            return fcntl_lock_shim(fd, cmd, fl);
        case F_SETLKW:
        case F_OFD_SETLKW:
            // Implement blocking by polling the shim.
            while (true) {
                result = fcntl_lock_shim(fd, cmd, fl);
                if (result == 0) {
                    return 0;
                }
                if (errno != EAGAIN) {
                    return result;
                }
                usleep(10000);
            }
        default:
            return sym_fcntl(fd, cmd, arg);
    }
}
