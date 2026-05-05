#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <err.h>

int main() {
    long page_size = sysconf(_SC_PAGESIZE);
    void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        err(EXIT_FAILURE, "mmap");
    }

    printf("Testing MAP_LOCKED...\n");
    void *addr2 = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    if (addr2 == MAP_FAILED) {
        if (errno == EAGAIN || errno == EPERM) {
             printf("mmap(MAP_LOCKED) failed with EAGAIN/EPERM, skipping.\n");
        } else {
             err(EXIT_FAILURE, "mmap(MAP_LOCKED) failed");
        }
    } else {
        printf("Success: mmap(MAP_LOCKED) returned valid address.\n");
        munmap(addr2, page_size);
    }

    printf("Testing mlock()...\n");
    if (mlock(addr, page_size) != 0) {
        if (errno == ENOSYS) {
            errx(EXIT_FAILURE, "mlock() not implemented (ENOSYS)");
        }
        if (errno == EPERM) {
            // Some systems limit mlock to root or have low RLIMIT_MEMLOCK
            printf("mlock() failed with EPERM, skipping residency check (try as root?)\n");
            goto cleanup;
        }
        err(EXIT_FAILURE, "mlock failed");
    }

    printf("Success: mlock() returned 0.\n");

    // We can also test munlock
    if (munlock(addr, page_size) != 0) {
        err(EXIT_FAILURE, "munlock failed");
    }
    printf("Success: munlock() returned 0.\n");

cleanup:
    munmap(addr, page_size);
    return 0;
}
