#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <err.h>

int main() {
    const char *shm_name = "/wslcompat_shm_test";
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) err(EXIT_FAILURE, "shm_open");

    if (ftruncate(fd, 4096) == -1) err(EXIT_FAILURE, "ftruncate");

    int *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) err(EXIT_FAILURE, "mmap");

    *ptr = 0x12345678;

    printf("Testing SHM sharing between processes...\n");

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Child
        if (*ptr != 0x12345678) {
            fprintf(stderr, "Child saw wrong value in SHM: 0x%x\n", *ptr);
            exit(1);
        }
        *ptr = 0x87654321;
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WEXITSTATUS(status) != 0) {
        errx(EXIT_FAILURE, "Child failed SHM test");
    }

    if (*ptr != 0x87654321) {
        errx(EXIT_FAILURE, "Parent didn't see child's update to SHM: 0x%x", *ptr);
    }

    printf("Success: SHM correctly shared and updated between processes.\n");

    munmap(ptr, 4096);
    close(fd);
    shm_unlink(shm_name);

    return 0;
}
