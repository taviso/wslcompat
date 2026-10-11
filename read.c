#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "shim.h"
#include "synth.h"
#include "logging.h"
#include "tunables.h"

SHIM_INIT(read, write, lseek, pread, pwrite, readv, writev, preadv, pwritev,
          fsync, fdatasync, ftruncate, fchmod, fchown, fallocate,
          fgetxattr, fsetxattr, flistxattr, fremovexattr);

// Thin pass-throughs that route to synth dispatch on the kernel's
// O_PATH canary errno. See SYNTH_SHIM / SYNTH_SHIM_E in synth.h.
ssize_t read(int fd, void *buf, size_t count)
{
    wsltrace("%d %p %lu", fd, buf, count);
    SYNTH_SHIM(read, fd, buf, count);
}

// FORTIFY_SOURCE rewrites read()/poll()/etc. to their __*_chk
// counterparts at compile time when the destination buffer size is
// known.
ssize_t __read_chk(int fd, void *buf, size_t nbytes, size_t buflen)
{
    wsltrace("fd=%d buf=%p nbytes=%zu buflen=%zu", fd, buf, nbytes, buflen);
    if (nbytes > buflen)
        wslcompat_chk_fail();
    return read(fd, buf, nbytes);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    wsltrace("%d %p %lu", fd, buf, count);
    SYNTH_SHIM(write, fd, buf, count);
}

off_t lseek(int fd, off_t offset, int whence)
{
    wsltrace("%d %lu %d", fd, offset, whence);
    SYNTH_SHIM(lseek, fd, offset, whence);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    wsltrace("%d %p %zu %ld", fd, buf, count, (long)offset);
    SYNTH_SHIM(pread, fd, buf, count, offset);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    wsltrace("%d %p %zu %ld", fd, buf, count, (long)offset);
    SYNTH_SHIM(pwrite, fd, buf, count, offset);
}

// O_PATH yields EPERM for the vectored I/O calls, not EBADF.
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    wsltrace("%d %p %d", fd, iov, iovcnt);
    SYNTH_SHIM_E(readv, EPERM, fd, iov, iovcnt);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    wsltrace("%d %p %d", fd, iov, iovcnt);
    SYNTH_SHIM_E(writev, EPERM, fd, iov, iovcnt);
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
    wsltrace("%d %p %d %ld", fd, iov, iovcnt, (long)offset);
    SYNTH_SHIM_E(preadv, EPERM, fd, iov, iovcnt, offset);
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
    wsltrace("%d %p %d %ld", fd, iov, iovcnt, (long)offset);
    SYNTH_SHIM_E(pwritev, EPERM, fd, iov, iovcnt, offset);
}

int fsync(int fd)
{
    wsltrace("%d", fd);
    SYNTH_SHIM(fsync, fd);
}

int fdatasync(int fd)
{
    wsltrace("%d", fd);
    SYNTH_SHIM(fdatasync, fd);
}

// O_PATH yields EINVAL for ftruncate.
int ftruncate(int fd, off_t length)
{
    wsltrace("%d %ld", fd, (long)length);
    SYNTH_SHIM_E(ftruncate, EINVAL, fd, length);
}

int fchmod(int fd, mode_t mode)
{
    wsltrace("%d 0%o", fd, mode);
    SYNTH_SHIM(fchmod, fd, mode);
}

int fchown(int fd, uid_t owner, gid_t group)
{
    wsltrace("%d %u %u", fd, owner, group);
    SYNTH_SHIM(fchown, fd, owner, group);
}

// O_PATH yields EOPNOTSUPP for fallocate on a regular file.
int fallocate(int fd, int mode, off_t offset, off_t len)
{
    wsltrace("%d %d %ld %ld", fd, mode, (long)offset, (long)len);
    SYNTH_SHIM_E(fallocate, EOPNOTSUPP, fd, mode, offset, len);
}

ssize_t fgetxattr(int fd, const char *name, void *value, size_t size)
{
    wsltrace("%d %s %p %zu", fd, name, value, size);
    SYNTH_SHIM(fgetxattr, fd, name, value, size);
}

int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    wsltrace("%d %s %p %zu %d", fd, name, value, size, flags);
    SYNTH_SHIM(fsetxattr, fd, name, value, size, flags);
}

ssize_t flistxattr(int fd, char *list, size_t size)
{
    wsltrace("%d %p %zu", fd, list, size);
    SYNTH_SHIM(flistxattr, fd, list, size);
}

int fremovexattr(int fd, const char *name)
{
    wsltrace("%d %s", fd, name);
    SYNTH_SHIM(fremovexattr, fd, name);
}

ssize_t __pread_chk(int fd, void *buf, size_t nbytes, off_t offset, size_t buflen)
{
    wsltrace("fd=%d buf=%p nbytes=%zu offset=%ld buflen=%zu", fd, buf, nbytes, (long)offset, buflen);
    if (nbytes > buflen)
        wslcompat_chk_fail();
    return pread(fd, buf, nbytes, offset);
}

off_t lseek64(int fd, off_t offset, int whence) __attribute__((alias("lseek")));
ssize_t pread64(int fd, void *buf, size_t count, off_t offset) __attribute__((alias("pread")));
ssize_t pwrite64(int fd, const void *buf, size_t count, off_t offset) __attribute__((alias("pwrite")));
ssize_t preadv64(int fd, const struct iovec *iov, int iovcnt, off_t offset) __attribute__((alias("preadv")));
ssize_t pwritev64(int fd, const struct iovec *iov, int iovcnt, off_t offset) __attribute__((alias("pwritev")));
int ftruncate64(int fd, off_t length) __attribute__((alias("ftruncate")));
int fallocate64(int fd, int mode, off_t offset, off_t len) __attribute__((alias("fallocate")));
ssize_t __pread64_chk(int fd, void *buf, size_t nbytes, off_t offset, size_t buflen) __attribute__((alias("__pread_chk")));
