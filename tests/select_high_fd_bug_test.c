/* sel_swap_in scans fds 0..min(nfds, FD_SETSIZE) for synth fds to
 * translate. When a synth fd *itself* lands at a number >= FD_SETSIZE
 * (possible when the process already holds >1024 open fds), the scan
 * never sees it, n_swaps stays 0, and sel_dispatch returns the
 * original EPERM -- select() fails outright instead of reporting
 * readiness for any fd in the set.
 *
 * The stock fd_set is only FD_SETSIZE bits wide; reading or writing
 * past that boundary with FD_ISSET/FD_SET/FD_CLR is UB, so the shim
 * cannot safely extend its scan. This test documents that limitation.
 *
 * Without the shim: synth_open is unavailable, the high pipe fd alone
 * is watched, and select returns 1 (pass).
 * With the shim: synth_open creates an O_PATH wrap at a high fd number,
 * the canary fires (EPERM), but the swap scan misses the synth fd
 * entirely -> select returns -1/EPERM (fail, XFIXME).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <unistd.h>

#include "testutil.h"
#include "../synth.h"

static int shared_real_fd = -1;
static int shared_passthru_cb(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e; (void)kind;
    return shared_real_fd;
}
static const struct synth_ops shared_ops = { .passthru = shared_passthru_cb };

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < FD_SETSIZE + 32) {
        rl.rlim_cur = FD_SETSIZE + 32;
        rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            printf("  SKIP: cannot raise RLIMIT_NOFILE\n");
            return 0;
        }
    }

    int p[2];
    if (pipe(p) < 0) err(EXIT_FAILURE, "pipe");

    int high = -1;
    for (int i = 0; i < FD_SETSIZE + 10; i++) {
        high = dup(p[0]);
        if (high >= FD_SETSIZE + 5) break;
    }
    if (high < FD_SETSIZE) {
        printf("  SKIP: cannot get high fd\n");
        return 0;
    }

    if (write(p[1], "x", 1) != 1) err(EXIT_FAILURE, "write");

    int (*synth_open)(int, const struct synth_ops *, void *) =
        dlsym(RTLD_DEFAULT, "wslcompat_synth_open");
    int synth_fd = -1;
    if (synth_open) {
        int dummy[2];
        if (pipe(dummy) < 0) err(EXIT_FAILURE, "pipe");
        shared_real_fd = dummy[0];
        synth_fd = synth_open(O_CLOEXEC, &shared_ops, NULL);
    }

    size_t set_bytes = (high + 64) / 8;
    fd_set *r = calloc(1, set_bytes);
    if (!r) err(EXIT_FAILURE, "calloc");

    unsigned long *bits = (unsigned long *)r;
    bits[high / (8 * sizeof(unsigned long))] |=
        (1UL << (high % (8 * sizeof(unsigned long))));

    int maxfd = high;
    if (synth_fd >= 0) {
        bits[synth_fd / (8 * sizeof(unsigned long))] |=
            (1UL << (synth_fd % (8 * sizeof(unsigned long))));
        if (synth_fd > maxfd) maxfd = synth_fd;
    }

    struct timeval tv = { 1, 0 };
    int rv = select(maxfd + 1, r, NULL, NULL, &tv);
    int expected_rv = (synth_fd >= 0) ? 2 : 1;

    CHECK(rv == expected_rv,
          "select rv == %d (got %d, errno=%d %s)",
          expected_rv, rv, errno, strerror(errno));

    TEST_REPORT();
}
