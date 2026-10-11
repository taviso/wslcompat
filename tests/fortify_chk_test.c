/* Verify FORTIFY_SOURCE-fortified read/poll calls hit our __*_chk
 * interposers and dispatch through SYNTH_SHIM correctly.
 *
 * Background: -D_FORTIFY_SOURCE=2 rewrites read(fd, buf, n) into
 * __read_chk(fd, buf, n, sizeof(buf)) when the buffer size is known
 * at compile time. glibc's __read_chk tail-calls the *internal* alias
 * __read (intra-DSO direct jump), bypassing our public `read`
 * interposer entirely. Same pattern for poll/ppoll. The fix is to
 * interpose __read_chk / __poll_chk / __pread_chk ourselves.
 *
 * This test uses a normal temporary file and a normal pipe to verify
 * that the intercepted __read_chk/__poll_chk correctly pass the data
 * through without breaking native glibc behavior natively, while routing
 * into the shim under LD_PRELOAD.
 */
#pragma GCC optimize("O2")
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 2
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

#include "testutil.h"

extern ssize_t __read_chk(int fd, void *buf, size_t nbytes, size_t buflen);
extern int     __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout,
                          size_t fdslen);
extern ssize_t __pread_chk(int fd, void *buf, size_t nbytes, off_t offset, size_t buflen);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int synth = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
    if (synth >= 0) {
        // Subtest 1: explicit __read_chk
        char buf[64];
        ssize_t r = __read_chk(synth, buf, 4, sizeof(buf));
        CHECK(r > 0, "__read_chk explicit -> %zd %s", r, r < 0 ? strerror(errno) : "");

        // Subtest 2: FORTIFY-rewritten read()
        lseek(synth, 0, SEEK_SET);
        r = read(synth, buf, 4);
        CHECK(r > 0, "fortified read() -> %zd %s", r, r < 0 ? strerror(errno) : "");

        // Subtest 3: explicit __pread_chk
        r = __pread_chk(synth, buf, 4, 0, sizeof(buf));
        CHECK(r > 0, "__pread_chk explicit -> %zd %s", r, r < 0 ? strerror(errno) : "");

        // Subtest 4: FORTIFY-rewritten pread()
        r = pread(synth, buf, 4, 0);
        CHECK(r > 0, "fortified pread() -> %zd %s", r, r < 0 ? strerror(errno) : "");

        close(synth);
    } else {
        STATUS("/proc/sys/vm/mmap_min_addr absent, skipping read/pread tests");
    }

    int pfd[2];
    if (pipe(pfd) == 0) {
        // Subtest 5: explicit __poll_chk (pipe empty, expect timeout)
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int r = __poll_chk(&p, 1, 50, sizeof(p));
        CHECK(r == 0, "__poll_chk explicit -> %d (expected 0 timeout)", r);

        // Subtest 6: FORTIFY-rewritten poll()
        if (write(pfd[1], "!", 1) != 1)
            err(EXIT_FAILURE, "pipe write");
        r = poll(&p, 1, 1000);
        CHECK(r == 1 && (p.revents & POLLIN), "fortified poll() -> %d", r);

        close(pfd[0]);
        close(pfd[1]);
    } else {
        err(EXIT_FAILURE, "pipe");
    }

    TEST_REPORT();
}
