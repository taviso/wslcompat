/* Verify F_DUPFD_CLOEXEC works on WSL1.
 *
 * synth.c's wslcompat_synth_relocate_probe relies on this to move
 * our internal probe fds out of the way when a caller's close,
 * dup2, or dup3 targets one of them. F_DUPFD_CLOEXEC was added in
 * Linux 2.6.24 (2008), but WSL1's coverage of fcntl commands is
 * patchy enough that we check it directly here. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    char tmpl[] = "/tmp/wslcompat_fdupfd_XXXXXX";
    int orig = mkstemp(tmpl);
    if (orig < 0) {
        printf("FAIL: mkstemp: %s\n", strerror(errno));
        return 1;
    }
    unlink(tmpl);

    /* Request a fd >= some number well above whatever the runtime
     * grabbed for stdio + mkstemp -- this is the pattern synth.c
     * uses to push probes out of the way of dup2 targets. */
    int min_fd = orig + 100;
    errno = 0;
    int dup = fcntl(orig, F_DUPFD_CLOEXEC, min_fd);
    CHECK(dup >= 0,
          "F_DUPFD_CLOEXEC(orig=%d, min=%d) -> %d (errno=%s)",
          orig, min_fd, dup, dup < 0 ? strerror(errno) : "ok");

    if (dup >= 0) {
        CHECK(dup >= min_fd,
              "returned fd %d is >= min %d", dup, min_fd);

        int flags = fcntl(dup, F_GETFD);
        CHECK(flags >= 0 && (flags & FD_CLOEXEC),
              "F_GETFD -> 0x%x (FD_CLOEXEC %s)",
              flags,
              (flags >= 0 && (flags & FD_CLOEXEC)) ? "set" : "missing");

        /* Verify the dup'd fd actually shares the underlying file:
         * write through orig, read back through dup. */
        const char *msg = "hello";
        ssize_t w = pwrite(orig, msg, 5, 0);
        char buf[8] = {0};
        ssize_t r = pread(dup, buf, 5, 0);
        CHECK(w == 5 && r == 5 && memcmp(buf, msg, 5) == 0,
              "dup'd fd refs same inode (w=%zd r=%zd buf=\"%s\")",
              w, r, buf);

        close(dup);
    }

    close(orig);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
