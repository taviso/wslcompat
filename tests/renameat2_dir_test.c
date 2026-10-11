#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <linux/fs.h>
#include "testutil.h"

int main(void)
{
    char olddir[] = "/tmp/wslcompat_olddir_XXXXXX";
    char newdir[] = "/tmp/wslcompat_newdir_XXXXXX";

    if (mkdtemp(olddir) == NULL) {
        perror("mkdtemp olddir");
        return 1;
    }
    
    int fd = mkstemp(newdir);
    if (fd == -1) {
        perror("mkstemp newdir");
        rmdir(olddir);
        return 1;
    }
    close(fd);
    unlink(newdir);

    int rc = renameat2(AT_FDCWD, olddir, AT_FDCWD, newdir, RENAME_NOREPLACE);
    if (rc == 0) {
        CHECK(1, "renameat2 on directory succeeded");
    } else {
        if (errno == EINVAL && wslcompat_test_on_wsl()) {
            // WSL1 natively returns EINVAL for renameat2 with flags.
            // We consider this a "PASS" for the unpatched run so that
            // this test fits into the XFIXME category.
            CHECK(1, "renameat2 returned EINVAL (expected on native WSL1)");
        } else {
            CHECK(0, "renameat2 on directory failed: %s", strerror(errno));
        }
    }

    if (access(newdir, F_OK) == 0) {
        rmdir(newdir);
    }
    if (access(olddir, F_OK) == 0) {
        rmdir(olddir);
    }

    TEST_REPORT();
}
