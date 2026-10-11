/* Synth slot release under fork-no-exec: open all 64 wraps, fork a
 * child that pause()s holding the inherited OFDs, parent closes its
 * copies, parent opens one more.
 *
 * The flock probe alone would fail here -- the child's inherited
 * caller_fd still holds LOCK_SH on each wrap's inode -- so each
 * post_close falls back to walking /proc/self/fd to confirm that no
 * fd in *this* process still references the wrap. That walk lets
 * the parent release its 64 producer refs even while the child
 * holds inherited copies, so the 65th open succeeds.
 *
 * Same code path that makes pty_master_pollhup_after_child work
 * (parent's hidden real_fd dup of a tty slave gets dropped when the
 * user closes the wrap, even though a forked child still holds the
 * slave). Without the walk, asyncio/emerge hang.
 *
 * The mmap_min_addr handler is pure-synthetic (no tty anchor) so
 * the test isolates the slot-lifecycle question. dlsym probe makes
 * the unshimmed run skip cleanly.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH   "/proc/sys/vm/mmap_min_addr"
#define NWRAPS 64

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
    alarm(20);

    if (dlsym(RTLD_DEFAULT, "wslcompat_synth_open") == NULL) {
        printf("  SKIP: libwslcompat not loaded\n");
        return 0;
    }

    int fds[NWRAPS];
    int opened = 0;
    for (int i = 0; i < NWRAPS; i++) {
        fds[i] = open(PATH, O_RDONLY);
        if (fds[i] < 0) break;
        opened++;
    }
    printf("--- opened %d/%d wraps before fork ---\n", opened, NWRAPS);

    if (opened < NWRAPS) {
        printf("  SKIP: table not fully claimable (other slots in use)\n");
        for (int i = 0; i < opened; i++) close(fds[i]);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        /* Holds inherited wrap OFDs across the parent's closes.
         * Parent SIGTERMs us when the probe is done. */
        pause();
        _exit(0);
    }

    /* Every close is contested (child still references the OFDs) so
     * each post_close falls through to the walk fallback, finds no
     * other in-process ref, and releases. */
    for (int i = 0; i < opened; i++)
        close(fds[i]);

    /* The probe: a 65th open after closing all 64. Succeeds iff the
     * walk in post_close released the slots while the child was
     * holding inherited copies. */
    int probe = open(PATH, O_RDONLY);
    CHECK(probe >= 0,
          "open after fork+close(all 64): %s",
          probe >= 0 ? "ok" : strerror(errno));
    if (probe >= 0) close(probe);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
