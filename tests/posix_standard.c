#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

static const char *tmp_path = "/tmp/posix_standard_test";

#define TEST_PASS(msg) printf("  [PASS] %s\n", msg)
#define TEST_FAIL(msg, reason) do {             \
    printf("  [FAIL] %s: %s\n", msg, reason);   \
    result++;                                   \
} while (false)

static bool set_lock(int fd, short type, off_t start, off_t len, bool wait) {
    struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl) == 0;
}

int test_basic_conflict() {
    int result = 0;
    printf("--- Subtest: Basic Cross-Process Conflict ---\n");
    int fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    set_lock(fd, F_WRLCK, 0, 0, false); // Whole file

    if (fork() == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        if (!set_lock(fd2, F_WRLCK, 0, 0, false)) {
            TEST_PASS("Process B blocked by Process A (Whole file)");
        } else {
            TEST_FAIL("Process B blocked by Process A", "Conflict not detected!");
        }
        _exit(0);
    }
    wait(NULL);
    close(fd);
    return result;
}

int test_ranges() {
    int result = 0;
    printf("\n--- Subtest: Range Enforcement ---\n");
    int fd = open(tmp_path, O_RDWR);
    set_lock(fd, F_WRLCK, 100, 10, false);

    if (fork() == 0) {
        int result = 0;
        int fd2 = open(tmp_path, O_RDWR);
        if (set_lock(fd2, F_WRLCK, 200, 10, false)) {
            TEST_PASS("Unrelated ranges allowed");
        } else {
            TEST_FAIL("Unrelated ranges allowed", "Incorrectly blocked");
        }

        if (!set_lock(fd2, F_WRLCK, 105, 10, false)) {
            TEST_PASS("Overlapping ranges blocked");
        } else {
            TEST_FAIL("Overlapping ranges blocked", "Incorrectly allowed!");
        }
        _exit(result);
    }
    wait(&result);
    close(fd);
    return WEXITSTATUS(result);
}

int test_inheritance() {
    int result = 0;
    printf("\n--- Subtest: Fork Inheritance ---\n");
    int fd = open(tmp_path, O_RDWR);
    set_lock(fd, F_WRLCK, 0, 0, false);

    if (fork() == 0) {
        // POSIX locks are NOT inherited. Child should be blocked by parent.
        // Wait, on Linux the child is blocked. On WSL1 we suspect it's inherited.
        int fd2 = open(tmp_path, O_RDWR);
        if (!set_lock(fd2, F_WRLCK, 0, 0, false)) {
            TEST_PASS("Child blocked by parent (Standard Linux)");
        } else {
            TEST_FAIL("Child blocked by parent", "Child incorrectly inherited/ignored lock!");
        }
        _exit(0);
    }
    wait(NULL);
    close(fd);
    return result;
}

int test_close_wipes_all() {
    int result = 0;
    printf("\n--- Subtest: close() wipes all locks ---\n");
    int fd1 = open(tmp_path, O_RDWR);
    int fd2 = open(tmp_path, O_RDWR);

    set_lock(fd1, F_WRLCK, 0, 0, false);
    printf("  Acquired lock on fd1. Closing fd2...\n");
    close(fd2); // Closing ANY descriptor to the file wipes all locks for the process!

    if (fork() == 0) {
        int fd3 = open(tmp_path, O_RDWR);
        if (set_lock(fd3, F_WRLCK, 0, 0, false)) {
            TEST_PASS("Lock was wiped by close of unrelated FD (Standard Linux)");
        } else {
            TEST_FAIL("Lock was wiped by close of unrelated FD", "Lock persisted!");
        }
        _exit(0);
    }
    wait(NULL);
    close(fd1);
    return result;
}

int main() {
    int result = 0;
    printf("POSIX Standard Semantics Test\n");
    result += test_basic_conflict();
    result += test_ranges();
    result += test_inheritance();
    result += test_close_wipes_all();
    unlink(tmp_path);
    return !! result;
}
