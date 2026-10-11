#ifndef WSLCOMPAT_UTILS_H
#define WSLCOMPAT_UTILS_H

#include <errno.h>

static inline void wslcompat_restore_errno(int *saved_errno) {
    errno = *saved_errno;
}

/* 
 * Restores errno when the current scope exits. Safe to use with return/goto/break.
 * Usage:
 * {
 *     preserve_errno();
 *     if (error) return -1; // errno is automatically restored
 * }
 */
#define _PRESERVE_ERRNO_CONCAT_IMPL(x, y) x##y
#define _PRESERVE_ERRNO_CONCAT(x, y) _PRESERVE_ERRNO_CONCAT_IMPL(x, y)

#define preserve_errno()                                                    \
    __attribute__((cleanup(wslcompat_restore_errno)))                       \
    __attribute__((unused))                                                 \
    int _PRESERVE_ERRNO_CONCAT(_wslcompat_saved_errno_, __COUNTER__) = errno

typedef int (*wslcompat_fd_visit_fn)(int fd, void *user);

void wslcompat_walk_proc_self_fd(wslcompat_fd_visit_fn visit, void *user);

#endif
