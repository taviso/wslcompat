#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <errno.h>
#include <string.h>

int main() {
    char tmpfile[] = "/tmp/unlinked_test_XXXXXX";
    int fd1 = mkstemp(tmpfile);
    if (fd1 == -1) { perror("mkstemp"); return 1; }

    printf("1. Acquiring LOCK_EX on fd1...\n");
    if (flock(fd1, LOCK_EX) == -1) { perror("flock fd1"); return 1; }

    printf("2. Unlinking %s while holding lock...\n", tmpfile);
    if (unlink(tmpfile) == -1) { perror("unlink"); return 1; }

    char procpath[64];
    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd1);
    printf("3. Opening %s as fd2...\n", procpath);
    int fd2 = open(procpath, O_RDONLY);
    if (fd2 == -1) {
        perror("  [FAIL] open /proc/self/fd/N failed for unlinked file");
        return 1;
    }

    printf("4. Testing LOCK_EX on fd2 (should fail if it's a new OFD for the same inode)...\n");
    if (flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
        printf("  [PASS] Conflict detected on unlinked file via /proc.\n");
    } else {
        printf("  [FAIL] No conflict! Either same OFD (wrong) or different inode (wrong).\n");
    }

    close(fd1);
    close(fd2);
    return 0;
}
