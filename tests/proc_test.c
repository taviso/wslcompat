#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <errno.h>
#include <string.h>

int main() {
    int failures = 0;
    char tmpfile[] = "/tmp/proc_test_XXXXXX";
    int fd1 = mkstemp(tmpfile);
    if (fd1 == -1) { perror("mkstemp"); return 1; }
    // DO NOT UNLINK YET

    printf("1. Acquiring LOCK_EX on fd1...\n");
    if (flock(fd1, LOCK_EX) == -1) { perror("flock fd1"); return 1; }

    char procpath[64];
    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd1);
    printf("2. Opening %s as fd2...\n", procpath);
    int fd2 = open(procpath, O_RDWR);
    if (fd2 == -1) { perror("open procpath"); return 1; }

    printf("3. Testing LOCK_EX on fd2 (should fail)...\n");
    if (flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
        printf("  [PASS] fd2 correctly blocked by fd1.\n");
    } else {
        printf("  [FAIL] fd2 was NOT blocked by fd1!\n");
        failures++;
    }

    printf("4. Verifying fd1 still holds its lock...\n");
    int fd3 = open(tmpfile, O_RDWR);
    if (flock(fd3, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
        printf("  [PASS] fd1 still blocks fd3.\n");
    } else {
        printf("  [FAIL] fd1 no longer blocks fd3!\n");
        failures++;
    }
    close(fd3);

    printf("5. Downgrading fd1 to LOCK_SH...\n");
    if (flock(fd1, LOCK_SH) == -1) { perror("flock fd1 sh"); return 1; }

    printf("6. Testing LOCK_EX on fd2 (should fail)...\n");
    if (flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
        printf("  [PASS] fd2 (EX) blocked by fd1 (SH).\n");
    } else {
        printf("  [FAIL] fd2 (EX) NOT blocked by fd1 (SH)!\n");
        failures++;
    }

    printf("7. Testing LOCK_SH on fd2 (should succeed)...\n");
    if (flock(fd2, LOCK_SH | LOCK_NB) == 0) {
        printf("  [PASS] fd2 (SH) allowed alongside fd1 (SH).\n");
        flock(fd2, LOCK_UN);
    } else {
        printf("  [FAIL] fd2 (SH) blocked by fd1 (SH)!\n");
        failures++;
    }

    printf("8. Closing fd2 and checking fd1...\n");
    close(fd2);
    fd3 = open(tmpfile, O_RDWR);
    if (flock(fd3, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
        printf("  [PASS] fd1 still blocks fd3 after fd2 was closed.\n");
    } else {
        printf("  [FAIL] fd1 lost its lock!\n");
        failures++;
    }

    close(fd1);
    close(fd3);
    unlink(tmpfile);
    return failures > 0 ? 1 : 0;
}
