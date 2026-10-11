#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

static char tmp_path[] = "/tmp/flock_standard_XXXXXX";

#define TEST_PASS(msg) do { printf("  [PASS] %s\n", msg); _exit(0); } while (0)
#define TEST_FAIL(msg, reason) do { printf("  [FAIL] %s: %s\n", msg, reason); _exit(1); } while (0)

static int failures;

static void reap_child(void) {
    int status;
    wait(&status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failures++;
}

void test_exclusivity() {
    printf("--- Subtest: Exclusivity ---\n");
    int fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    flock(fd, LOCK_EX);

    if (fork() == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        if (flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
            TEST_PASS("Process B blocked by Process A (LOCK_EX)");
        } else {
            TEST_FAIL("Process B blocked by Process A", "Conflict not detected!");
        }
    }
    reap_child();
    close(fd);
}

void test_shared() {
    printf("\n--- Subtest: Shared Locks ---\n");
    int fd = open(tmp_path, O_RDWR);
    flock(fd, LOCK_SH);

    if (fork() == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        if (flock(fd2, LOCK_SH | LOCK_NB) == 0) {
            TEST_PASS("Process B allowed shared lock alongside Process A");
        } else {
            TEST_FAIL("Process B allowed shared lock", "Incorrectly blocked!");
        }
    }
    reap_child();
    close(fd);
}

void test_inheritance() {
    printf("\n--- Subtest: Fork Inheritance (Shared OFD) ---\n");
    int fd = open(tmp_path, O_RDWR);
    flock(fd, LOCK_EX);

    if (fork() == 0) {
        // Child inherits the FD AND the lock ownership (shared OFD).
        // It should be able to upgrade/downgrade/release it.
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            TEST_PASS("Child inherited lock ownership (Shared OFD)");
        } else {
            TEST_FAIL("Child inherited lock ownership", "Blocked from its own inherited lock!");
        }
    }
    reap_child();
    close(fd);
}

void test_close_persistence() {
    printf("\n--- Subtest: close() of dup doesn't release ---\n");
    int fd1 = open(tmp_path, O_RDWR);
    int fd2 = dup(fd1);

    flock(fd1, LOCK_EX);
    printf("  Acquired lock on fd1. Closing fd2 (dup)...\n");
    close(fd2);

    if (fork() == 0) {
        int fd3 = open(tmp_path, O_RDWR);
        if (flock(fd3, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
            TEST_PASS("Lock persisted after close of dup (Standard BSD)");
        } else {
            TEST_FAIL("Lock persisted after close of dup", "Lock was lost!");
        }
    }
    reap_child();
    close(fd1);
}

int main() {
    printf("flock() Standard Semantics Test\n");

    int seed = mkstemp(tmp_path);
    if (seed < 0) { perror("mkstemp"); return 1; }
    close(seed);

    test_exclusivity();
    test_shared();
    test_inheritance();
    test_close_persistence();
    unlink(tmp_path);
    return failures > 0 ? 1 : 0;
}
