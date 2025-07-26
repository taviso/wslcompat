#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/XXXXXX";
    int fd1, fd2;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0, // Lock the entire file
        .l_pid    = 0,
    };

    fd1 = mkstemp(tmpfile);
    fd2 = dup(fd1);

    unlink(tmpfile);

    // Acquire the lock on the first file descriptor
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "fcntl() F_OFD_SETLK failed");
    }

    // Attempt to acquire a conflicting lock on the second file descriptor.
    if (fcntl(fd2, F_OFD_SETLK, &fl) == -1) {
        if (errno == EAGAIN || errno == EACCES) {
            printf("As expected, failed to acquire lock on fd2: Lock is held by fd1.\n");
        } else {
            perror("fcntl lock fd2");
        }
    } else {
        printf("Error: Acquired lock on fd2 when it should have failed.\n");
    }

    // Release the lock on fd1
    printf("\nReleasing lock on fd1...\n");
    fl.l_type = F_UNLCK;
    if (fcntl(fd1, F_OFD_SETLK, &fl) == -1) {
        perror("fcntl unlock fd1");
    }

    // Now, try to acquire the lock on fd2 again. It should succeed.
    printf("Attempting to acquire lock on fd2 again...\n");
    fl.l_type = F_WRLCK;
    if (fcntl(fd2, F_OFD_SETLK, &fl) == -1) {
        perror("fcntl lock fd2 after release");
    } else {
        printf("Successfully acquired lock on fd2.\n");
    }

    // Clean up
    close(fd1);
    close(fd2);

    return 0;
}
