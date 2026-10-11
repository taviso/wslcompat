#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <libgen.h>

static char oldname[64] = "/tmp/wslcompat_rename_old_XXXXXX";
static char newname[64] = "/tmp/wslcompat_rename_new_XXXXXX";

static void cleanup(void)
{
    unlink(oldname);
    unlink(newname);
}

int main(int argc, char **argv)
{
    int oldfd, newfd;

    atexit(cleanup);

    if ((oldfd = mkstemp(oldname)) == -1)
        err(EXIT_FAILURE, "mkstemp old");
    close(oldfd);

    if ((newfd = mkstemp(newname)) == -1)
        err(EXIT_FAILURE, "mkstemp new");
    close(newfd);

    printf("testing RENAME_NOREPLACE with existing destination\n");
    if (renameat2(AT_FDCWD, oldname, AT_FDCWD, newname, RENAME_NOREPLACE) == 0) {
        errx(EXIT_FAILURE, "renameat2 RENAME_NOREPLACE unexpectedly succeeded");
    }

    if (errno != EEXIST) {
        err(EXIT_FAILURE, "renameat2 RENAME_NOREPLACE failed with unexpected error");
    }

    printf("testing RENAME_NOREPLACE with non-existing destination\n");
    unlink(newname);

    if (renameat2(AT_FDCWD, oldname, AT_FDCWD, newname, RENAME_NOREPLACE) != 0) {
        err(EXIT_FAILURE, "renameat2 RENAME_NOREPLACE failed");
    }

    if (access(newname, F_OK) != 0) {
        err(EXIT_FAILURE, "newname does not exist after rename");
    }
    if (access(oldname, F_OK) == 0) {
        errx(EXIT_FAILURE, "oldname still exists after rename");
    }

    unlink(newname);
    printf("testing rename to self with RENAME_NOREPLACE (should fail)\n");
    
    strcpy(oldname, "/tmp/wslcompat_rename_self_XXXXXX");
    if ((oldfd = mkstemp(oldname)) == -1)
        err(EXIT_FAILURE, "mkstemp self");
    close(oldfd);

    // According to Linux behavior, RENAME_NOREPLACE fails even if renaming to self.
    if (renameat2(AT_FDCWD, oldname, AT_FDCWD, oldname, RENAME_NOREPLACE) == 0) {
        errx(EXIT_FAILURE, "renameat2 to self unexpectedly succeeded");
    }

    if (errno != EEXIST) {
        err(EXIT_FAILURE, "renameat2 to self failed with unexpected error");
    }

    unlink(oldname);
    printf("all tests pass\n");
    return 0;
}
