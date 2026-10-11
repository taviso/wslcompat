/* Regression test for the synth-table GC under table-full pressure.
 *
 * synth_register reclaims slots whose wrap has gone dead system-wide but
 * whose producer ref was never dropped -- a "ghost". A ghost forms when
 * post_close defers the release (a dup still holds the wrap) and the
 * surviving ref is then closed by a path that does NOT run the close shim
 * (a raw syscall, as Go and static binaries do). Nothing re-probes such a
 * slot, so without the GC it leaks until the installer exits; 64 of them
 * exhaust the table and open() returns ENFILE.
 *
 * Sister to synth_fork_release_test / synth_fork_pause_test, but those
 * exercise the *walk* (a live holder elsewhere); with the walk present
 * the installer releases at its own close, so no slot is ever deferred
 * into a ghost. This isolates the GC by manufacturing one ghost per round:
 *   - open a wrap            (entry, refcount 1, LOCK_SH on its OFD)
 *   - dup it                 (shared OFD, so the wrap stays alive)
 *   - close() the original   (shimmed -> post_close defers: the dup holds it)
 *   - raw close the dup      (frees the OFD, NO post_close -> dead+occupied)
 * Looping well past the 64-slot table must keep succeeding: each full-table
 * open triggers the GC sweep, which flock-reprobes the ghosts, finds them
 * dead, and reclaims one. Without the GC the 65th round hits ENFILE.
 *
 * TESTS (fails unshimmed, passes shimmed): WSL1 has no real
 * /proc/sys/vm/mmap_min_addr, so the unpatched run can't open it and fails
 * -- what the xfl_ rule expects. `make linux` runs it on a real kernel
 * where the file exists and the open/dup/close churn just works, so it
 * passes there too.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

#define PATH    "/proc/sys/vm/mmap_min_addr"
#define ROUNDS  200   /* >> the 64-slot table: a ghost leak would ENFILE
                       * long before this */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20);

    printf("--- ghost-slot churn (%d rounds, 64-slot table) ---\n", ROUNDS);
    int max_round = ROUNDS;
    for (int round = 0; round < ROUNDS; round++) {
        int a = open(PATH, O_RDONLY);
        if (a < 0) {
            printf("  FAIL: round %d open: %s\n", round, strerror(errno));
            max_round = round;
            break;
        }
        int b = dup(a);              /* shares the OFD -> wrap stays live */
        if (b < 0) {
            printf("  FAIL: round %d dup: %s\n", round, strerror(errno));
            close(a);
            max_round = round;
            break;
        }
        close(a);                    /* shimmed: post_close defers (b holds it) */
        syscall(SYS_close, b);       /* raw: frees the OFD, no post_close -> ghost */
    }
    CHECK(max_round == ROUNDS,
          "completed %d/%d ghost-churn rounds without ENFILE",
          max_round, ROUNDS);

    int probe = open(PATH, O_RDONLY);
    CHECK(probe >= 0, "fresh open after churn: %s",
          probe < 0 ? strerror(errno) : "ok");
    if (probe >= 0)
        close(probe);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
