/* Exercises poll/ppoll at the boundaries of the alloca'd inotify
 * splice path: empty, single-fd, at the MAX_POLL_FDS cap, just over
 * the cap (bypass path), and the NULL-pointer corner. The bypass
 * path must keep returning the right thing -- async re-probe wake
 * is allowed to silently degrade for the huge case, but correctness
 * is not.
 *
 * MAX_POLL_FDS in poll.c is 1024; we go to 2048 to provoke bypass. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static void test_zero_nfds(void)
{
    printf("--- poll(NULL, 0, timeout) ---\n");

    int r = poll(NULL, 0, 50);
    CHECK(r == 0, "poll(NULL, 0, 50ms) -> %d (expected 0)", r);

    struct timespec t = { 0, 50 * 1000 * 1000 };
    r = ppoll(NULL, 0, &t, NULL);
    CHECK(r == 0, "ppoll(NULL, 0, 50ms) -> %d (expected 0)", r);
}

static void test_single_fd_pipe(void)
{
    printf("--- single pipe fd ---\n");

    int p[2];
    if (pipe(p) != 0) err(EXIT_FAILURE, "pipe");

    struct pollfd pfd = { .fd = p[0], .events = POLLIN };
    int r = poll(&pfd, 1, 50);
    CHECK(r == 0, "poll(pipe, empty, 50ms) -> %d (expected 0)", r);

    if (write(p[1], "x", 1) != 1) err(EXIT_FAILURE, "write");
    pfd.revents = 0;
    r = poll(&pfd, 1, 500);
    CHECK(r == 1 && (pfd.revents & POLLIN),
          "poll(pipe, 1 byte, 500ms) -> %d revents=0x%x", r, pfd.revents);

    close(p[0]);
    close(p[1]);
}

// Build nfds pipe reads, write to a known one, verify poll surfaces
// exactly that fd. `nfds` may exceed the inotify-splice cap; in that
// case the shim's bypass path runs and async re-probe wake is lost,
// but correctness on the user's fds must survive.
static void test_many_pipes(nfds_t nfds, const char *label)
{
    printf("--- %s (nfds=%lu) ---\n", label, (unsigned long)nfds);

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < nfds * 2 + 16) {
        rl.rlim_cur = nfds * 2 + 16;
        if (rl.rlim_cur > rl.rlim_max)
            rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            printf("  SKIP: cannot raise RLIMIT_NOFILE to %lu (%m)\n",
                   (unsigned long)rl.rlim_cur);
            return;
        }
    }

    struct pollfd *pfds = calloc(nfds, sizeof(*pfds));
    int *write_ends = calloc(nfds, sizeof(int));
    if (pfds == NULL || write_ends == NULL) err(EXIT_FAILURE, "calloc");

    for (nfds_t i = 0; i < nfds; i++) {
        int p[2];
        if (pipe(p) != 0) {
            // ENFILE/EMFILE this far in -- bail with what we have.
            printf("  SKIP: pipe() failed at i=%lu (%m), reducing nfds\n",
                   (unsigned long)i);
            nfds = i;
            break;
        }
        pfds[i].fd     = p[0];
        pfds[i].events = POLLIN;
        write_ends[i]  = p[1];
    }

    int r = poll(pfds, nfds, 50);
    CHECK(r == 0, "poll(%lu fds, empty, 50ms) -> %d (expected 0)",
          (unsigned long)nfds, r);

    // Write into a fd near the end so a buggy nfds bump would miss it.
    nfds_t target = nfds > 0 ? nfds - 1 : 0;
    if (nfds > 0 && write(write_ends[target], "x", 1) != 1)
        err(EXIT_FAILURE, "write");

    for (nfds_t i = 0; i < nfds; i++) pfds[i].revents = 0;
    r = poll(pfds, nfds, 1000);
    CHECK(r == 1 && pfds[target].revents & POLLIN,
          "poll(%lu fds, last-fd written, 1s) -> %d, target.revents=0x%x",
          (unsigned long)nfds, r, pfds[target].revents);

    for (nfds_t i = 0; i < nfds; i++) {
        close(pfds[i].fd);
        close(write_ends[i]);
    }
    free(pfds);
    free(write_ends);
}

static void test_timeout_zero_and_infinite(void)
{
    printf("--- timeout corners ---\n");

    int p[2];
    if (pipe(p) != 0) err(EXIT_FAILURE, "pipe");
    struct pollfd pfd = { .fd = p[0], .events = POLLIN };

    // timeout=0 -> immediate return
    int r = poll(&pfd, 1, 0);
    CHECK(r == 0, "poll(timeout=0, empty pipe) -> %d (expected 0)", r);

    // timeout=-1 -> block until the forked child writes.
    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        usleep(100 * 1000);
        if (write(p[1], "x", 1) != 1) _exit(2);
        _exit(0);
    }
    pfd.revents = 0;
    r = poll(&pfd, 1, -1);
    int status;
    waitpid(pid, &status, 0);
    CHECK(r == 1 && (pfd.revents & POLLIN),
          "poll(timeout=-1) unblocked by child write -> %d revents=0x%x",
          r, pfd.revents);

    close(p[0]);
    close(p[1]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    test_zero_nfds();
    test_single_fd_pipe();
    test_timeout_zero_and_infinite();
    test_many_pipes(1023, "splice path (just below cap)");
    test_many_pipes(2048, "bypass path (above cap)");

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
