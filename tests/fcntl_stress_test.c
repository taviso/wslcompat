#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

static const char *tmp_path = "/tmp/fcntl_stress_test";
static int failures = 0;

#define ASSERT_LOCK_SUCCESS(fd, type, start, len, msg) do { \
    struct flock fl = { .l_type = (type), .l_whence = SEEK_SET, .l_start = (start), .l_len = (len) }; \
    if (fcntl((fd), F_SETLK, &fl) == -1) { \
        printf("  [FAIL] %s: Expected success, but got error: %s\n", (msg), strerror(errno)); \
        failures++; \
    } else { \
        printf("  [PASS] %s: Success.\n", (msg)); \
    } \
} while(0)

#define ASSERT_LOCK_BLOCKED(fd, type, start, len, msg) do { \
    struct flock fl = { .l_type = (type), .l_whence = SEEK_SET, .l_start = (start), .l_len = (len) }; \
    if (fcntl((fd), F_SETLK, &fl) == 0) { \
        printf("  [FAIL] %s: Expected blockage, but got success!\n", (msg)); \
        failures++; \
    } else if (errno == EAGAIN || errno == EACCES) { \
        printf("  [PASS] %s: Blocked correctly.\n", (msg)); \
    } else { \
        printf("  [FAIL] %s: Expected EAGAIN, but got error: %s\n", (msg), strerror(errno)); \
        failures++; \
    } \
} while(0)

void run_cross_process_tests(int fd) {
    printf("--- Stage 1: Cross-Process Enforcement ---\n");
    
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 10 };
    fcntl(fd, F_SETLK, &fl);

    pid_t pid = fork();
    if (pid == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        ASSERT_LOCK_SUCCESS(fd2, F_WRLCK, 200, 10, "Unrelated range [200, 210]");
        ASSERT_LOCK_BLOCKED(fd2, F_WRLCK, 100, 10, "Exact same range [100, 110]");
        ASSERT_LOCK_BLOCKED(fd2, F_WRLCK, 105, 10, "Overlapping range [105, 115]");
        ASSERT_LOCK_BLOCKED(fd2, F_WRLCK, 95, 10,  "Overlapping range [95, 105]");
        ASSERT_LOCK_SUCCESS(fd2, F_RDLCK, 250, 10, "Unrelated Read range [250, 260]");
        _exit(failures > 0 ? 1 : 0);
    }
    int status;
    wait(&status);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) failures++;
    
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
}

void run_intra_process_ofd_tests(int fd) {
    printf("\n--- Stage 2: Same Process, Different OFD (open() again) ---\n");
    int fd2 = open(tmp_path, O_RDWR);

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 10 };
    fcntl(fd, F_SETLK, &fl);

    ASSERT_LOCK_SUCCESS(fd2, F_WRLCK, 100, 10, "Overlapping range (same PID, diff OFD)");

    close(fd2);
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
}

void run_reporting_tests(int fd) {
    printf("\n--- Stage 3: F_GETLK Reporting Accuracy ---\n");
    
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 1234, .l_len = 5678 };
    fcntl(fd, F_SETLK, &fl);

    pid_t pid = fork();
    if (pid == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        struct flock query = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        fcntl(fd2, F_GETLK, &query);
        
        printf("  Conflicting lock reported by kernel:\n");
        printf("    Type: %s\n", (query.l_type == F_WRLCK ? "WRITE" : "READ"));
        printf("    Start: %lld (Expected: 1234)\n", (long long)query.l_start);
        printf("    Len: %lld (Expected: 5678)\n", (long long)query.l_len);
        printf("    PID: %d (Expected: %d)\n", query.l_pid, getppid());

        if (query.l_start != 1234 || query.l_len != 5678 || query.l_pid != getppid()) {
            printf("  [FAIL] Reporting mismatch detected.\n");
            _exit(1);
        }
        printf("  [PASS] Reporting accuracy verified.\n");
        _exit(0);
    }
    int status;
    wait(&status);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) failures++;

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
}

void run_merging_splitting_tests(int fd) {
    printf("\n--- Stage 4: Merging and Splitting ---\n");

    struct flock fl1 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 10 };
    struct flock fl2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 110, .l_len = 10 };
    fcntl(fd, F_SETLK, &fl1);
    fcntl(fd, F_SETLK, &fl2);

    pid_t pid = fork();
    if (pid == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        struct flock q = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 105, .l_len = 1 };
        fcntl(fd2, F_GETLK, &q);
        printf("  Merged range report: Start=%lld, Len=%lld\n", (long long)q.l_start, (long long)q.l_len);
        if (q.l_start != 100 || q.l_len != 20) {
             printf("  [FAIL] Merge reporting mismatch.\n");
             _exit(1);
        }
        _exit(0);
    }
    int status;
    wait(&status);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) failures++;

    printf("  Unlocking middle [105, 115]...\n");
    struct flock unl = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 105, .l_len = 10 };
    fcntl(fd, F_SETLK, &unl);

    pid_t pid2 = fork();
    if (pid2 == 0) {
        int fd2 = open(tmp_path, O_RDWR);
        struct flock q1 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 5 };
        struct flock q2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 105, .l_len = 5 };
        struct flock q3 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 115, .l_len = 5 };
        
        fcntl(fd2, F_GETLK, &q1);
        fcntl(fd2, F_GETLK, &q2);
        fcntl(fd2, F_GETLK, &q3);

        printf("  Split check: [100, 105] %s, [105, 110] %s, [115, 120] %s\n",
               (q1.l_type != F_UNLCK ? "LOCKED" : "UNLOCKED"),
               (q2.l_type != F_UNLCK ? "LOCKED" : "UNLOCKED"),
               (q3.l_type != F_UNLCK ? "LOCKED" : "UNLOCKED"));
        
        if (q1.l_type == F_UNLCK || q2.l_type != F_UNLCK || q3.l_type == F_UNLCK) {
            printf("  [FAIL] Split verification failed.\n");
            _exit(1);
        }
        _exit(0);
    }
    wait(&status);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) failures++;
}

int main() {
    int fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) { perror("open"); return 1; }

    run_cross_process_tests(fd);
    run_intra_process_ofd_tests(fd);
    run_reporting_tests(fd);
    run_merging_splitting_tests(fd);

    close(fd);
    unlink(tmp_path);

    if (failures > 0) {
        printf("\nTOTAL FAILURES: %d\n", failures);
        return 1;
    }
    printf("\nALL TESTS PASSED.\n");
    return 0;
}
