#define _GNU_SOURCE
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(copy_file_range);

ssize_t copy_file_range(int fd_in, loff_t *off_in, int fd_out,
                        loff_t *off_out, size_t len, unsigned int flags)
{
    char buf[4096];
    ssize_t total = 0;
    ssize_t r;
    loff_t local_off_in;
    loff_t local_off_out;

    r = sym_next(copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
    if (r != -1 || errno != ENOSYS || wslcompat_passthru_self())
        return r;

    wsldbg("copy_file_range(fd_in=%d, fd_out=%d, len=%zu)", fd_in, fd_out, len);

    // If offset pointers are NULL, point them to local variables initialized
    // to the current file offsets. This avoids branching in the read/write loop.
    local_off_in = lseek(fd_in, 0, SEEK_CUR);
    local_off_out = lseek(fd_out, 0, SEEK_CUR);

    wsltrace("fd_in=%d@%ld, fd_out=%d@%ld", fd_in, local_off_in, fd_out, local_off_out);

    off_in = off_in ? off_in : &local_off_in;
    off_out = off_out ? off_out : &local_off_out;

    if (*off_in == (loff_t)-1 || *off_out == (loff_t)-1) {
        return -1;
    }

    while (len > 0) {
        size_t chunk = sizeof(buf);

        if (len < chunk) {
            chunk = len;
        }

        if ((r = pread(fd_in, buf, chunk, *off_in)) < 0) {
            return total > 0 ? total : -1;
        }

        if (r == 0)
            break;

        for (ssize_t written = 0; written < r;) {
            ssize_t w = pwrite(fd_out, buf + written, r - written, *off_out + written);

            if (w < 0) {
                return total > 0 ? total : -1;
            }

            written += w;
        }

        *off_in += r;
        *off_out += r;
        total += r;
        len -= r;
    }

    // Only update the file descriptor offsets if the original pointers were NULL
    if (off_in == &local_off_in) {
        lseek(fd_in, *off_in, SEEK_SET);
    }
    if (off_out == &local_off_out) {
        lseek(fd_out, *off_out, SEEK_SET);
    }

    return total;
}
