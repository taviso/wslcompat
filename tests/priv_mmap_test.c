/* Cross-privilege mmap testing.
 *
 * Checks if root is correctly allowed to map at NULL using MAP_FIXED and
 * MAP_FIXED_NOREPLACE natively.
 *
 * XFIXME: Our mmap polyfill for MAP_FIXED_NOREPLACE currently blocks mapping
 * at NULL unconditionally (returns EPERM) because the probe technique
 * is impossible when the hint is NULL. Native Linux/WSL allows it for root.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/user.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <err.h>
#include "testutil.h"

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        warnx("must be run as root (try `sudo make`)");
        return 1;
    }

    void *ptr = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(ptr == NULL, "root MAP_FIXED at NULL succeeds");
    if (ptr != MAP_FAILED) {
        munmap(ptr, PAGE_SIZE);
    }

    void *noreplace = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
    
    // On WSL1 natively, this flag is unsupported and returns EOPNOTSUPP.
    // XFIXME expects the native run to pass, so we must allow EOPNOTSUPP natively.
    if (noreplace == MAP_FAILED && errno == EOPNOTSUPP && wslcompat_test_on_wsl()) {
        CHECK(1, "root MAP_FIXED_NOREPLACE at NULL natively unsupported on WSL1");
    } else {
        CHECK(noreplace == NULL, "root MAP_FIXED_NOREPLACE at NULL succeeds");
        if (noreplace != MAP_FAILED) {
            munmap(noreplace, PAGE_SIZE);
        }
    }

    TEST_REPORT();
}
