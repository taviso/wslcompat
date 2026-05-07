#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/ofd_interop_XXXXXX";
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    printf("--- Test: OFD vs POSIX Interoperability ---\n");
    int fd = mkostemp(tmpfile, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    printf("1. Acquiring OFD lock via shim...\n");
    if (fcntl(fd, F_OFD_SETLK, &fl) == -1) err(EXIT_FAILURE, "fcntl SETLK");

    if (fork() == 0) {
        int fd2 = open(tmpfile, O_RDWR);
        struct flock query = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        
        printf("2. Child (NOT using shim) querying via standard POSIX F_GETLK...\n");
        // Use the raw syscall to bypass our library wrapper.
        if (syscall(SYS_fcntl, fd2, F_GETLK, &query) == -1) err(EXIT_FAILURE, "direct fcntl");

        if (query.l_type == F_UNLCK) {
            printf("FAIL: Standard POSIX fcntl could not see our OFD lock!\n");
            exit(1);
        } else {
            printf("PASS: Standard POSIX fcntl correctly detected our lock.\n");
            exit(0);
        }
    }

    int status; wait(&status);
    unlink(tmpfile);
    return WEXITSTATUS(status);
}
