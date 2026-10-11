/* Isolated test of the flock-on-O_PATH probe machinery synth.c uses
 * to detect last-close on the user's synth fd.
 *
 * The model:
 *   1. flock(path_fd, LOCK_SH) on an O_PATH fd takes a real lock on
 *      that OFD (WSL1-specific behaviour; on stock Linux flock on
 *      O_PATH returns EBADF).
 *   2. open("/proc/self/fd/path_fd", O_RDONLY) creates an INDEPENDENT
 *      OFD on the same inode, with NO inherited lock state.
 *   3. LOCK_SH held by anyone on the inode blocks LOCK_EX on probe_fd.
 *   4. When the last OFD that held LOCK_SH is gone (closed in any
 *      process), LOCK_SH is released; probe_fd's LOCK_EX|LOCK_NB
 *      succeeds.
 *
 * This file exercises (1)..(4) against the bare kernel -- no shim
 * loaded. The point is to enumerate the multi-process patterns where
 * the probe correctly fires (kernel cooperates) so that any failure
 * in our shim's USE of the probe shows up as "the kernel says dead
 * but we never noticed".
 *
 * The shim currently runs the probe exactly once per close call. In
 * fork scenarios where the synth O_PATH was inherited, the probe at
 * close-time correctly says "alive" (child still has ref), but the
 * shim never re-probes, so even after the child exits / closes, our
 * private real_fd (in tty.c et al) stays open forever.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testutil.h"

// Build a (back_fd, path_fd, probe_fd) triple mimicking synth_open
// but without the shim. Returns path_fd via *out_path, probe_fd via
// *out_probe; closes back_fd before returning.
static void setup_probe(int *out_path, int *out_probe)
{
    char tmpl[] = "/tmp/probesem.XXXXXX";
    int back_fd = mkstemp(tmpl);
    if (back_fd < 0)
        err(EXIT_FAILURE, "mkstemp");
    unlink(tmpl);

    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", back_fd);
    int path_fd = open(proc, O_PATH | O_CLOEXEC);
    if (path_fd < 0)
        err(EXIT_FAILURE, "reopen as O_PATH");

    if (flock(path_fd, LOCK_SH | LOCK_NB) != 0)
        err(EXIT_FAILURE, "flock LOCK_SH on path_fd");

    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", path_fd);
    int probe_fd = open(proc, O_RDONLY | O_CLOEXEC);
    if (probe_fd < 0)
        err(EXIT_FAILURE, "open probe_fd");

    close(back_fd);

    *out_path = path_fd;
    *out_probe = probe_fd;
}

// Try LOCK_EX|LOCK_NB on probe_fd and release immediately if taken.
// Returns 1 if the lock was acquired (== "synth dead"), 0 otherwise.
static int probe_dead(int probe_fd)
{
    if (flock(probe_fd, LOCK_EX | LOCK_NB) == 0) {
        flock(probe_fd, LOCK_UN);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------
// Single-process scenarios -- baseline that the model holds at all.
// ---------------------------------------------------------------
static void test_single_close(void)
{
    int path_fd, probe_fd;
    setup_probe(&path_fd, &probe_fd);

    CHECK(!probe_dead(probe_fd), "[single] alive while path_fd held");
    close(path_fd);
    CHECK(probe_dead(probe_fd), "[single] dead after path_fd close");

    close(probe_fd);
}

static void test_dup3_then_close(void)
{
    int path_fd, probe_fd;
    setup_probe(&path_fd, &probe_fd);

    int user_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (user_fd < 0)
        err(EXIT_FAILURE, "open /dev/null");

    if (dup3(path_fd, user_fd, O_CLOEXEC) < 0)
        err(EXIT_FAILURE, "dup3");
    close(path_fd);

    CHECK(!probe_dead(probe_fd),
          "[dup3] alive while user_fd holds dup3'd OFD");
    close(user_fd);
    CHECK(probe_dead(probe_fd),
          "[dup3] dead after user_fd close");

    close(probe_fd);
}

// ---------------------------------------------------------------
// Multi-process scenarios. In every fork case the child has a slot
// for itself that lets the parent wait until something predictable
// has happened before checking the probe.
// ---------------------------------------------------------------

// A: child inherits the synth, child exits without touching it,
// parent reaps. Probe should fire after the wait4 returns.
static void test_fork_exit(void)
{
    int path_fd, probe_fd;
    setup_probe(&path_fd, &probe_fd);

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");
    if (pid == 0)
        _exit(0);

    // Parent: close before waiting so only the child's inherited ref
    // is keeping the synth OFD alive.
    close(path_fd);

    CHECK(!probe_dead(probe_fd),
          "[fork-exit] alive between parent-close and child-exit");

    waitpid(pid, NULL, 0);
    CHECK(probe_dead(probe_fd),
          "[fork-exit] dead after child exit + wait4 returned");

    close(probe_fd);
}

// B: child inherits the synth, child explicitly closes its inherited
// ref, child stays alive (blocked on a sync pipe). Probe should fire
// based on child's close alone -- but our shim has no event hook for
// "another process closed a file"; this is the hard scenario.
static void test_fork_child_closes(void)
{
    int path_fd, probe_fd;
    setup_probe(&path_fd, &probe_fd);

    int sync_to_child[2], sync_to_parent[2];
    if (pipe(sync_to_child) != 0 || pipe(sync_to_parent) != 0)
        err(EXIT_FAILURE, "pipe");

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        close(sync_to_child[1]);
        close(sync_to_parent[0]);
        char b;
        // Wait for parent to say "I closed mine, your turn".
        read(sync_to_child[0], &b, 1);
        close(path_fd);
        // Tell parent we've closed; then block forever so probe must
        // detect the close without any "child exited" signal.
        write(sync_to_parent[1], "x", 1);
        pause();
        _exit(0);
    }

    close(sync_to_child[0]);
    close(sync_to_parent[1]);

    close(path_fd);
    write(sync_to_child[1], "x", 1);

    char b;
    read(sync_to_parent[0], &b, 1);

    CHECK(probe_dead(probe_fd),
          "[fork-childclose] dead after child closed its inherited ref");

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(probe_fd);
    close(sync_to_child[1]);
    close(sync_to_parent[0]);
}

// C: child execs (FD_CLOEXEC on the inherited synth fires). Probe
// should be live or dead depending on whether the inherited fd had
// CLOEXEC and whether the parent has dropped its ref by then.
static void test_fork_exec_cloexec(void)
{
    int path_fd, probe_fd;
    setup_probe(&path_fd, &probe_fd);

    // Set CLOEXEC on path_fd so the child's inherited copy disappears
    // on exec. tty.c's tty_install passes O_CLOEXEC into synth_open,
    // so this matches what the shim actually does.
    int flags = fcntl(path_fd, F_GETFD);
    fcntl(path_fd, F_SETFD, flags | FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        // Exec something trivial. CLOEXEC closes the inherited synth
        // fd as part of the exec.
        execl("/bin/true", "true", (char *)NULL);
        _exit(127);
    }

    close(path_fd);
    waitpid(pid, NULL, 0);

    CHECK(probe_dead(probe_fd),
          "[fork-exec-cloexec] dead after child exec dropped CLOEXEC fd");

    close(probe_fd);
}

// The whole probe model rests on flock-on-O_PATH being honoured.
// Stock Linux returns EBADF; only WSL1 cooperates. On Linux skip the
// suite cleanly so the XPASS rule keeps working everywhere.
static int flock_on_o_path_works(void)
{
    char tmpl[] = "/tmp/probesem_probe.XXXXXX";
    int back_fd = mkstemp(tmpl);
    if (back_fd < 0) return 0;
    unlink(tmpl);

    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", back_fd);
    int path_fd = open(proc, O_PATH | O_CLOEXEC);
    close(back_fd);
    if (path_fd < 0) return 0;

    int ok = (flock(path_fd, LOCK_SH | LOCK_NB) == 0);
    close(path_fd);
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    wslcompat_test_require_or_skip(flock_on_o_path_works(),
                                   "flock on O_PATH not supported");

    test_single_close();
    test_dup3_then_close();
    test_fork_exit();
    test_fork_child_closes();
    test_fork_exec_cloexec();

    TEST_REPORT();
}
