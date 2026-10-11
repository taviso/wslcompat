#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

static int try_lock(int fd, int type)
{
    struct flock fl = {
        .l_type   = type,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };
    return fcntl(fd, F_SETLK, &fl);
}

static void must_lock(int fd, int type)
{
    if (try_lock(fd, type) == -1) err(EXIT_FAILURE, "F_SETLK type=%d", type);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    char path[] = "/tmp/fcntl_upgrade_XXXXXX";
    int fd = mkostemp(path, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    int failures = 0;

    printf("--- Same-fd upgrade RDLCK -> WRLCK ---\n");
    must_lock(fd, F_RDLCK);
    if (try_lock(fd, F_WRLCK) == -1) {
        printf("  FAIL: upgrade rejected: %s\n", strerror(errno));
        failures++;
    } else {
        printf("  PASS\n");
    }
    must_lock(fd, F_UNLCK);

    printf("--- Same-fd downgrade WRLCK -> RDLCK ---\n");
    must_lock(fd, F_WRLCK);
    if (try_lock(fd, F_RDLCK) == -1) {
        printf("  FAIL: downgrade rejected: %s\n", strerror(errno));
        failures++;
    } else {
        printf("  PASS\n");
    }
    must_lock(fd, F_UNLCK);

    printf("--- Cross-process: upgrade blocked while peer holds RDLCK ---\n");
    must_lock(fd, F_RDLCK);
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        int fd2 = open(path, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");
        struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
        if (fcntl(fd2, F_SETLK, &fl) == -1) err(EXIT_FAILURE, "child F_RDLCK");
        sleep(2);
        fl.l_type = F_UNLCK;
        fcntl(fd2, F_SETLK, &fl);
        _exit(0);
    }
    sleep(1);
    if (try_lock(fd, F_WRLCK) == 0) {
        printf("  FAIL: upgrade succeeded while child holds RDLCK\n");
        failures++;
    } else if (errno != EAGAIN && errno != EACCES) {
        printf("  FAIL: unexpected errno: %s\n", strerror(errno));
        failures++;
    } else {
        printf("  PASS: upgrade blocked (%s)\n", strerror(errno));
    }
    wait(NULL);
    must_lock(fd, F_UNLCK);

    close(fd);
    unlink(path);

    if (failures > 0) {
        printf("FAIL: %d mismatch(es)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
