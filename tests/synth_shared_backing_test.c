/* Verify sel_unwind correctly reflects readiness onto MULTIPLE synth
 * fds that share the same real backing fd. The tty handler dups its
 * real_fd per install so this never happens in practice today, but a
 * future shared-event-source handler could legitimately wrap several
 * synth entries onto one underlying fd.
 *
 * The single-pass sel_unwind that this test guards against would
 * clear the real_fd bit immediately after mirroring it onto the
 * first matching synth, so a second synth pointing at the same
 * backing would silently lose its event. The two-pass version (mirror
 * first, then clear) preserves both.
 *
 * The test reaches past the public ABI -- it calls
 * wslcompat_synth_open directly to mint synth fds backed by a custom
 * ops table whose passthru() returns a shared pipe fd. That symbol
 * is only present under the shim; for the unshimmed sys_ run we
 * dlsym() and skip on failure. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <err.h>

#include "../synth.h"

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static int shared_real_fd = -1;

static int shared_passthru_cb(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e;
    (void)kind;
    return shared_real_fd;
}

static const struct synth_ops shared_ops = {
    .passthru = shared_passthru_cb,
};

static void test_select_shared_backing(void)
{
    alarm(2);
    printf("--- select: two synth fds sharing one real backing ---\n");

    int (*synth_open)(int, const struct synth_ops *, void *) =
        dlsym(RTLD_DEFAULT, "wslcompat_synth_open");
    if (synth_open == NULL) {
        printf("  SKIP: libwslcompat not loaded\n");
        return;
    }

    int pfd[2];
    if (pipe(pfd) < 0) err(EXIT_FAILURE, "pipe");
    shared_real_fd = pfd[0];

    int s1 = synth_open(O_CLOEXEC, &shared_ops, NULL);
    int s2 = synth_open(O_CLOEXEC, &shared_ops, NULL);
    CHECK(s1 >= 0 && s2 >= 0, "synth_open both -> %d, %d", s1, s2);
    CHECK(s1 != s2, "  distinct synth fd numbers");

    if (write(pfd[1], "x", 1) != 1) err(EXIT_FAILURE, "pipe write");

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s1, &rfds);
    FD_SET(s2, &rfds);
    int maxfd = (s1 > s2 ? s1 : s2) + 1;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };

    int r = select(maxfd, &rfds, NULL, NULL, &tv);

    /* rv must equal the total bits set after the call (POSIX says
     * "total number of bits set in the bit masks"). Both synth fds
     * get marked ready, so rv must be 2 -- the kernel originally
     * counted 1 (the shared real backing), so the dispatcher must
     * have recounted to account for our added synth bits. */
    CHECK(r == 2, "select rv == 2 after unwind (got %d)", r);

    /* Both synth fds must reflect the readiness of their shared
     * backing. The single-pass sel_unwind would clear pfd[0]'s bit
     * after handling the first swap and miss the second. */
    CHECK(FD_ISSET(s1, &rfds), "  s1 (%d) marked ready", s1);
    CHECK(FD_ISSET(s2, &rfds), "  s2 (%d) marked ready", s2);

    /* And the real backing fd should be cleared from the output --
     * it wasn't in the caller's input set. */
    CHECK(!FD_ISSET(shared_real_fd, &rfds),
          "  real backing (%d) not leaked into output", shared_real_fd);

    close(s1); close(s2);
    close(pfd[0]); close(pfd[1]);
}

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: synth_shared_backing_test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    test_select_shared_backing();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
