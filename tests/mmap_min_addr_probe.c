/* Verify /proc/sys/vm/mmap_min_addr matches the kernel's actual enforcement.
 *
 * Real Linux exposes this as /proc/sys/vm/mmap_min_addr (usually 65536
 * on x86_64). WSL1 doesn't expose it at all -- without the wslcompat
 * shim this test fails because the procfs read errors out. With the
 * shim, the polyfilled value should match the value the WSL1 kernel
 * actually enforces.
 *
 * The probe walks MAP_FIXED upward in page-sized steps until the kernel
 * stops refusing, then compares against /proc. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

int main(void)
{
    unsigned long pagesize = sysconf(_SC_PAGESIZE);
    unsigned long probed = 0;
    unsigned long advertised = 0;
    unsigned long enforced = 0;
    char buf[64] = {0};
    int fd;

    fd = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
    CHECK(fd >= 0, "open(/proc/sys/vm/mmap_min_addr): %s", strerror(errno));

    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        CHECK(n > 0, "read(/proc/sys/vm/mmap_min_addr): %s", strerror(errno));
        close(fd);
        if (n > 0) {
            advertised = strtoul(buf, NULL, 10);
            printf("  INFO: /proc says %lu\n", advertised);

            /* Kernel enforces page granularity even when the sysctl is finer. */
            enforced = (advertised + pagesize - 1) & ~(pagesize - 1);
        }
    }

    for (unsigned long addr = pagesize; addr <= 16UL * 1024 * 1024; addr += pagesize) {
        void *p = mmap((void *)addr, pagesize, PROT_NONE,
                       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == (void *)addr) {
            probed = addr;
            munmap(p, pagesize);
            break;
        }
        if (errno != EPERM && errno != EACCES && errno != EINVAL) {
            printf("  INFO: mmap(%p) failed unexpectedly: %s\n",
                   (void *)addr, strerror(errno));
            break;
        }
    }

    CHECK(probed != 0, "found a page the kernel accepts below 16M");

    if (probed) {
        printf("  INFO: lowest accepted MAP_FIXED address = %lu (0x%lx)\n",
               probed, probed);
    }

    CHECK(probed == enforced,
          "probed (%lu) matches enforced /proc value (%lu)", probed, enforced);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
