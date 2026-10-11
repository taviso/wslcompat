/* XFIXME: punch list of per-fd ops where the SYNTH_DISPATCH canary
 * trick doesn't fire, so the shim never sees the call.
 *
 *   syncfs        -- kernel silently returns 0 on O_PATH (no canary)
 *   fremovexattr  -- kernel returns ENODATA, same as "attr missing"
 *   fchdir        -- kernel returns ENOTDIR, same as wrong-type fd
 *   getdents64    -- not yet shimmed; would need a directory synth type
 *   fstatfs       -- kernel returns 0 with backing-tmpfile stats
 *
 * Each subtest registers a passthru handler that increments a counter
 * when invoked. We then call the op and expect the counter to be > 0.
 * Today none dispatch -> counter stays 0 -> subtest fails -> the
 * whole test returns 1, which XFIXME inverts to pass.
 *
 * When any op starts dispatching, the test will return 0 and trip the
 * `! ./fix_*` wrapper, prompting graduation into XPASS.
 *
 * Unshimmed: dlsym misses, test skips. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <err.h>

#include "../synth.h"

static int failures;
static int subtests;
static int passes;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) { printf("  PASS: " fmt "\n", ##__VA_ARGS__); passes++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static int back_fd = -1;
static int passthru_count = 0;

static int gap_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e;
    (void)kind;
    passthru_count++;
    return back_fd;
}

static const struct synth_ops gap_ops = {
    .passthru = gap_passthru,
};

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int (*synth_open)(int, const struct synth_ops *, void *) =
        dlsym(RTLD_DEFAULT, "wslcompat_synth_open");
    if (synth_open == NULL) {
        printf("  SKIP: libwslcompat not loaded (no wslcompat_synth_open)\n");
        return 0;
    }

    char path[] = "/tmp/wslcompat_gaps_XXXXXX";
    back_fd = mkstemp(path);
    if (back_fd < 0) err(EXIT_FAILURE, "mkstemp");
    unlink(path);

    int synth = synth_open(O_CLOEXEC, &gap_ops, NULL);
    if (synth < 0) err(EXIT_FAILURE, "synth_open");

    char buf[64];

    passthru_count = 0;
    syncfs(synth);
    CHECK(passthru_count > 0, "syncfs(synth) dispatched (passthru count=%d)",
          passthru_count);

    passthru_count = 0;
    fremovexattr(synth, "user.wslcompat.nope");
    CHECK(passthru_count > 0, "fremovexattr(synth) dispatched (passthru count=%d)",
          passthru_count);

    passthru_count = 0;
    fchdir(synth);
    CHECK(passthru_count > 0, "fchdir(synth) dispatched (passthru count=%d)",
          passthru_count);

    passthru_count = 0;
    syscall(SYS_getdents64, synth, buf, sizeof(buf));
    CHECK(passthru_count > 0, "getdents64(synth) dispatched (passthru count=%d)",
          passthru_count);

    struct statfs sfs;
    passthru_count = 0;
    fstatfs(synth, &sfs);
    CHECK(passthru_count > 0, "fstatfs(synth) dispatched (passthru count=%d)",
          passthru_count);

    close(synth);
    close(back_fd);

    printf("\n%d/%d subtests passed, %d failed\n",
           passes, subtests, failures);
    if (passes > 0)
        printf("HOORAY: %d op(s) now dispatch -- move them out of the gap"
               " list and into XPASS coverage.\n", passes);
    return passes > 0 ? 0 : 1;
}
