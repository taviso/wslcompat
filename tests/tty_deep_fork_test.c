/* Deep fork chain stress: parent wraps a pty slave as stdin via
 * tcsetattr, then a child chain forks 10 levels deep. At each level
 * the chain process waitpid's its child so the chain stays linear and
 * the leaf stays alive at the bottom.
 *
 *   parent (root) -> chain[0] -> chain[1] -> ... -> chain[9] -> leaf
 *
 * Two processes with the same synth-wrapped stdin are concurrently
 * alive: root and leaf. The intermediate chain levels are blocked in
 * waitpid and effectively just hold the shared inherited fds.
 *
 * Sequence:
 *   1. Leaf signals "ready" so root knows the chain reached bottom.
 *   2. Leaf runs the battery: tcgetattr, ioctl TIOCGWINSZ,
 *      poll/select with no data (expect 0), re-tcsetattr (exercises
 *      tty_install on an already-wrapped fd), then a write-to-master
 *      + read-from-stdin round-trip with a unique marker.
 *   3. Leaf signals "done" with its failure count.
 *   4. Root runs the same battery with its own marker. Root's wrap
 *      must still be functional even though leaf has been touching
 *      the same OFD.
 *   5. Root waitpid's the chain head; chain unwinds.
 *
 * What this catches:
 *   - tty_install state corruption across deep fork chains.
 *   - real_fd hidden in each process's fd table getting clobbered or
 *     relocated wrong by intermediate forks.
 *   - Re-tcsetattr at depth 10 (already-wrapped fd) breaking the
 *     wrap.
 *   - Cross-process synth lifecycle: when leaf exits, the slot's
 *     other refs in root must not be wiped.
 *
 * XPASS expected once the synth fd machinery is robust. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "testutil.h"

#define FORK_DEPTH 10

static int run_battery(int master, const char *role, char marker)
{
    int start_failures = wslcompat_test_failures;

    struct termios got;
    CHECK(tcgetattr(0, &got) == 0, "[%s] tcgetattr: %m", role);

    struct winsize ws;
    CHECK(ioctl(0, TIOCGWINSZ, &ws) == 0, "[%s] ioctl TIOCGWINSZ: %m", role);

    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    int r = poll(&pfd, 1, 50);
    CHECK(r == 0, "[%s] poll(empty, 50ms) -> %d revents=0x%x", role, r, pfd.revents);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    struct timeval tv = { 0, 50 * 1000 };
    r = select(1, &rfds, NULL, NULL, &tv);
    CHECK(r == 0, "[%s] select(empty, 50ms) -> %d", role, r);

    // Re-tcsetattr on an already-wrapped fd: tty_install must not
    // corrupt the existing wrap.
    CHECK(tcsetattr(0, TCSANOW, &got) == 0, "[%s] tcsetattr re-wrap: %m", role);

    CHECK(write(master, &marker, 1) == 1, "[%s] write(master): %m", role);

    pfd.fd = 0; pfd.events = POLLIN; pfd.revents = 0;
    r = poll(&pfd, 1, 2000);
    CHECK(r == 1 && (pfd.revents & POLLIN), "[%s] poll after write -> %d revents=0x%x", role, r, pfd.revents);

    char buf = '?';
    ssize_t n = read(0, &buf, 1);
    CHECK(n == 1 && buf == marker, "[%s] read -> %zd byte=%c (expected %c)", role, n, buf, marker);

    return wslcompat_test_failures - start_failures;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0) err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL) err(EXIT_FAILURE, "ptsname");

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0) err(EXIT_FAILURE, "open slave");

    if (dup2(slave, 0) < 0) err(EXIT_FAILURE, "dup2 stdin");
    close(slave);

    // Wrap stdin via tty_install. Subsequent forks inherit the wrap.
    struct termios attrs;
    if (tcgetattr(0, &attrs) != 0) err(EXIT_FAILURE, "tcgetattr");
    attrs.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(0, TCSANOW, &attrs) != 0) err(EXIT_FAILURE, "tcsetattr");

    int leaf_ready[2], leaf_done[2];
    if (pipe(leaf_ready) != 0 || pipe(leaf_done) != 0)
        err(EXIT_FAILURE, "sync pipe");

    pid_t chain = fork();
    if (chain < 0) err(EXIT_FAILURE, "fork chain");

    if (chain == 0) {
        close(leaf_ready[0]);
        close(leaf_done[0]);

        for (int level = 0; level < FORK_DEPTH; level++) {
            pid_t c = fork();
            if (c < 0) _exit(99);
            if (c > 0) {
                int s;
                waitpid(c, &s, 0);
                _exit(WIFEXITED(s) ? WEXITSTATUS(s) : 100);
            }
        }

        // Leaf: 10 forks below the original chain process.
        if (write(leaf_ready[1], "x", 1) != 1) _exit(20);
        int fail = run_battery(master, "leaf", 'L');
        unsigned char b = (unsigned char)(fail > 255 ? 255 : fail);
        if (write(leaf_done[1], &b, 1) != 1) _exit(21);
        _exit(fail);
    }

    close(leaf_ready[1]);
    close(leaf_done[1]);

    char ready;
    if (read(leaf_ready[0], &ready, 1) != 1)
        err(EXIT_FAILURE, "wait leaf ready");

    unsigned char leaf_b;
    if (read(leaf_done[0], &leaf_b, 1) != 1)
        err(EXIT_FAILURE, "wait leaf done");
    int leaf_fail = leaf_b;

    int root_fail = run_battery(master, "root", 'R');

    int status;
    waitpid(chain, &status, 0);

    close(master);
    close(leaf_ready[0]);
    close(leaf_done[0]);

    int total = root_fail + leaf_fail;
    
    // In multi-process tests, child CHECK macros increment child-local counters.
    // Ensure the parent accounts for the child's failures too.
    wslcompat_test_failures += leaf_fail;
    wslcompat_test_subtests += 8; // leaf runs 8 CHECKs

    CHECK(total == 0, "stdin wrap survives %d-deep fork chain (root=%d leaf=%d chain_exit=%d)",
          FORK_DEPTH, root_fail, leaf_fail, WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    TEST_REPORT();
}
