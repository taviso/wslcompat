#ifndef WSLCOMPAT_INTERNAL_FD_H
#define WSLCOMPAT_INTERNAL_FD_H

#include <stdatomic.h>

// Register a library-internal fd that should be rescued when its number is
// reused by an application via close/dup2/dup3.
//
// The slot follows the following convention:
//
//    -2 = uninitialized (lazy-open will fire)
//    -1 = open failed
//  >= 0 = live fd.
//
// The relocator always resets to -2 on dup failure so the module's lazy-open
// path retries on next use.
// This is expected to be called from a constructor.
// Note that the registry is fixed-size, easy to increase if necessary.
void wslcompat_register_internal_fd(atomic_int *slot);

// If any registered fd equals targetfd, dup it to a fresh high-numbered
// FD_CLOEXEC fd. This is called before dup/close/etc so internal
// file descriptors can be rescued while targetfd is still valid.
void wslcompat_relocate_internal_fd(int targetfd);

#endif
