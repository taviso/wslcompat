#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>

#include "testutil.h"

int main(int argc, char *argv[], char *envp[])
{
    char *args[] = { "/bin/true", NULL };
    int ret;
    int status;
    pid_t pid;

    // 1. Invalid flags (should return EINVAL)
    errno = 0;
    ret = execveat(AT_FDCWD, "/bin/true", args, envp, 9999);
    CHECK(ret == -1 && errno == EINVAL, "invalid flags returns EINVAL (got ret=%d, errno=%d)", ret, errno);

    // 2. Path truncation / overflow (should return ENAMETOOLONG)
    char *long_path = malloc(PATH_MAX + 10);
    if (long_path) {
        memset(long_path, 'a', PATH_MAX + 5);
        long_path[PATH_MAX + 5] = '\0';
        errno = 0;
        ret = execveat(AT_FDCWD, long_path, args, envp, 0);
        CHECK(ret == -1 && errno == ENAMETOOLONG, "truncated path returns ENAMETOOLONG (got ret=%d, errno=%d)", ret, errno);
        free(long_path);
    }

    // 3. Invalid dirfd (should return EBADF)
    errno = 0;
    ret = execveat(9999, "true", args, envp, 0);
    CHECK(ret == -1 && errno == EBADF, "invalid dirfd returns EBADF (got ret=%d, errno=%d)", ret, errno);

    // 4. Empty path without AT_EMPTY_PATH (should return ENOENT)
    int dirfd = open("/bin", O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        errno = 0;
        ret = execveat(dirfd, "", args, envp, 0);
        CHECK(ret == -1 && errno == ENOENT, "empty path without AT_EMPTY_PATH returns ENOENT (got ret=%d, errno=%d)", ret, errno);
    } else {
        CHECK(0, "failed to open /bin");
    }

    // 5. AT_SYMLINK_NOFOLLOW on a symlink (should return ELOOP)
    unlink("test_symlink");
    if (symlink("/bin/true", "test_symlink") == 0) {
        errno = 0;
        ret = execveat(AT_FDCWD, "test_symlink", args, envp, AT_SYMLINK_NOFOLLOW);
        CHECK(ret == -1 && errno == ELOOP, "AT_SYMLINK_NOFOLLOW on symlink returns ELOOP (got ret=%d, errno=%d)", ret, errno);
        unlink("test_symlink");
    } else {
        CHECK(0, "failed to create test_symlink");
    }

    // 6. Success: AT_EMPTY_PATH
    int true_fd = open("/bin/true", O_RDONLY | O_CLOEXEC);
    if (true_fd >= 0) {
        pid = fork();
        if (pid == 0) {
            execveat(true_fd, "", args, envp, AT_EMPTY_PATH);
            exit(127);
        }
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "AT_EMPTY_PATH executes successfully");
        close(true_fd);
    } else {
        CHECK(0, "failed to open /bin/true");
    }

    // 7. Success: Relative path with valid dirfd
    if (dirfd >= 0) {
        pid = fork();
        if (pid == 0) {
            execveat(dirfd, "true", args, envp, 0);
            exit(127);
        }
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "relative path with dirfd executes successfully");
        close(dirfd);
    }

    // 8. Success: Absolute path
    pid = fork();
    if (pid == 0) {
        execveat(AT_FDCWD, "/bin/true", args, envp, 0);
        exit(127);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "absolute path executes successfully");

    TEST_REPORT();
}
