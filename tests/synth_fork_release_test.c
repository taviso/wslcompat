/* Regression test for synth-table slot lifecycle across fork.
 *
 * If the close shim's per-process release logic regresses (e.g. by
 * reverting to flock-only detection without the /proc/self/fd walk
 * fallback), the installer process's slots stay allocated whenever
 * a forked child still holds the inherited wrap OFD. With enough
 * fork-leak cycles the synth table fills and new opens return ENFILE.
 *
 * The mmap_min_addr handler is convenient because it's pure-synthetic
 * (no hidden anchor) -- this test isolates the slot-lifecycle question
 * from anything tty-specific. Probes via dlsym so the unshimmed run
 * skips: the real /proc file exists on native Linux and has no table.
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

#include "testutil.h"

#define PATH       "/proc/sys/vm/mmap_min_addr"
#define ROUNDS     200    /* enough cycles to overflow a 64-slot table if leaking */
#define BATCH      4      /* wraps opened per round */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20);

    if (dlsym(RTLD_DEFAULT, "wslcompat_synth_open") == NULL) {
        STATUS("  SKIP: libwslcompat not loaded");
        return 0;
    }

    /* Round: open BATCH wraps, fork, child sleeps holding inherited
     * wraps + parent closes its copies. If the installer (parent)
     * release path leaks slots when the child still holds the OFD,
     * a few hundred rounds will exhaust the 64-slot table. */
    STATUS("--- fork+close lifecycle (%d rounds x %d wraps) ---",
           ROUNDS, BATCH);

    int max_round_reached = ROUNDS;
    for (int round = 0; round < ROUNDS; round++) {
        int fds[BATCH];
        for (int i = 0; i < BATCH; i++) {
            fds[i] = open(PATH, O_RDONLY);
            if (fds[i] < 0) {
                CHECK(0, "round %d open[%d]: %s",
                       round, i, strerror(errno));
                max_round_reached = round;
                goto done_rounds;
            }
        }

        /* Fork a child that holds the inherited wraps. Parent must
         * close BEFORE child exits, otherwise the race resolves with
         * child gone first and flock LOCK_EX succeeds in parent --
         * the easy path we're not trying to test. Use a pipe to
         * sequence: parent closes, then signals child to exit. */
        int sync_pipe[2];
        if (pipe(sync_pipe) < 0) {
            CHECK(0, "round %d pipe: %s", round, strerror(errno));
            for (int i = 0; i < BATCH; i++) close(fds[i]);
            max_round_reached = round;
            goto done_rounds;
        }

        pid_t pid = fork();
        if (pid < 0) {
            CHECK(0, "round %d fork: %s", round, strerror(errno));
            for (int i = 0; i < BATCH; i++) close(fds[i]);
            close(sync_pipe[0]); close(sync_pipe[1]);
            max_round_reached = round;
            goto done_rounds;
        }
        if (pid == 0) {
            /* Child: wait for parent to signal, then exit. Child's
             * inherited wrap fds keep the OFD alive across parent's
             * close in the meantime. */
            close(sync_pipe[1]);
            char b;
            read(sync_pipe[0], &b, 1);
            _exit(0);
        }

        close(sync_pipe[0]);

        /* Parent closes its wraps while child still holds them. This
         * is the contested case: flock LOCK_EX fails, post_close must
         * fall through to the per-process walk to decide release. */
        for (int i = 0; i < BATCH; i++)
            close(fds[i]);

        /* Now signal child to exit. */
        write(sync_pipe[1], "x", 1);
        close(sync_pipe[1]);
        waitpid(pid, NULL, 0);
    }
done_rounds:

    CHECK(max_round_reached == ROUNDS,
          "completed %d/%d rounds without ENFILE", max_round_reached, ROUNDS);

    /* Smoke check: after all those rounds, we can still open a wrap.
     * If slots leaked, this would have already failed above; this
     * check just confirms the table isn't poisoned. */
    int probe = open(PATH, O_RDONLY);
    CHECK(probe >= 0,
          "fresh open after %d rounds: %s",
          ROUNDS, probe < 0 ? strerror(errno) : "ok");
    if (probe >= 0) close(probe);

    TEST_REPORT();
}
