/* Does vfork() share the fd table (CLONE_FILES semantics) on the
 * host kernel? Stock Linux says NO -- vfork shares VM only, not the
 * fd table; child's close()s don't affect the parent.
 *
 * If WSL1 happens to share CLONE_FILES via vfork it'd mean our libc-
 * destructor pattern (close any internal_fd at exit) is a latent
 * footgun across the codebase, because after exec the child's
 * destructor would yank our fds out from under the parent.
 *
 * Two probes:
 *   1. Vfork child closes a fd via raw SYS_close (bypasses our shim
 *      so no relocate machinery interferes). Parent then writes to
 *      its end of that pipe and sees whether it's still open.
 *   2. Vfork child opens a NEW pipe (low fd, will collide with parent
 *      if shared). After child _exit, parent checks if its known-good
 *      fds still resolve to what it expects.
 *
 * The test is informational -- it doesn't load the shim. We compare
 * native Linux vs WSL1 by running it on both.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

// Vfork child closes parent's write-end. If fd tables are shared,
// parent's write will fail with EBADF.
static void test_child_close_affects_parent(void)
{
    int pfd[2];
    if (pipe(pfd) != 0)
        err(EXIT_FAILURE, "pipe");
    int rfd = pfd[0], wfd = pfd[1];

    pid_t pid = vfork();
    if (pid < 0)
        err(EXIT_FAILURE, "vfork");
    if (pid == 0) {
        syscall(SYS_close, wfd);
        _exit(0);
    }
    waitpid(pid, NULL, 0);

    char b = 'x';
    ssize_t r = write(wfd, &b, 1);
    int errno_save = errno;
    CHECK(r == 1,
          "[child-close] parent's wfd still writable after vfork "
          "child closed it -> r=%zd errno=%d (%s)",
          r, r < 0 ? errno_save : 0, r < 0 ? strerror(errno_save) : "ok");

    close(rfd);
    if (r == 1)
        close(wfd);
}

// Vfork child does a destructor-style close on an inherited fd
// number. If fd tables are shared, the parent loses that fd silently
// even though the child execs immediately afterwards.
static void test_child_close_then_exec(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        err(EXIT_FAILURE, "open /dev/null");

    pid_t pid = vfork();
    if (pid < 0)
        err(EXIT_FAILURE, "vfork");
    if (pid == 0) {
        // The bug scenario: a child between vfork and exec runs a
        // destructor that closes an fd we want the parent to keep.
        syscall(SYS_close, fd);
        execl("/bin/true", "true", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    // Parent's fd should still point to /dev/null if fd tables are
    // independent. If shared, fstat fails with EBADF.
    struct stat st;
    int r = fstat(fd, &st);
    int errno_save = errno;
    CHECK(r == 0,
          "[child-close-exec] parent's fd still valid post vfork+close+exec "
          "-> fstat r=%d errno=%d (%s)",
          r, r < 0 ? errno_save : 0, r < 0 ? strerror(errno_save) : "ok");

    if (r == 0)
        close(fd);
}

// Sanity: a vfork child that doesn't touch fds should obviously leave
// the parent's table intact. Validates the harness isn't measuring
// something else.
static void test_baseline_no_close(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        err(EXIT_FAILURE, "open /dev/null");

    pid_t pid = vfork();
    if (pid < 0)
        err(EXIT_FAILURE, "vfork");
    if (pid == 0) {
        execl("/bin/true", "true", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    struct stat st;
    int r = fstat(fd, &st);
    CHECK(r == 0, "[baseline] parent's fd valid when child didn't touch fds");
    close(fd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    test_baseline_no_close();
    test_child_close_affects_parent();
    test_child_close_then_exec();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
