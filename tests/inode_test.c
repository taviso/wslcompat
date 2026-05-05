#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/inode_test_XXXXXX";
    char symlink_path[] = "/tmp/inode_test_symlink";
    struct stat st1, st2, st3;

    int fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");
    
    if (fstat(fd1, &st1) == -1) err(EXIT_FAILURE, "fstat fd1");

    int fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open tmpfile");

    if (fstat(fd2, &st2) == -1) err(EXIT_FAILURE, "fstat fd2");

    unlink(symlink_path);
    if (symlink(tmpfile, symlink_path) == -1) err(EXIT_FAILURE, "symlink");

    int fd3 = open(symlink_path, O_RDWR);
    if (fd3 == -1) err(EXIT_FAILURE, "open symlink");

    if (fstat(fd3, &st3) == -1) err(EXIT_FAILURE, "fstat fd3");

    printf("Comparing inodes and devices...\n");
    printf("FD1 (mkostemp): dev=%lu, ino=%lu\n", (unsigned long)st1.st_dev, (unsigned long)st1.st_ino);
    printf("FD2 (open):     dev=%lu, ino=%lu\n", (unsigned long)st2.st_dev, (unsigned long)st2.st_ino);
    printf("FD3 (symlink):  dev=%lu, ino=%lu\n", (unsigned long)st3.st_dev, (unsigned long)st3.st_ino);

    if (st1.st_dev != st2.st_dev || st1.st_ino != st2.st_ino) {
        errx(EXIT_FAILURE, "Failure: Inode/Dev mismatch between open() calls.");
    }

    if (st1.st_dev != st3.st_dev || st1.st_ino != st3.st_ino) {
        errx(EXIT_FAILURE, "Failure: Inode/Dev mismatch via symlink.");
    }

    printf("Success: Inode and Device ID are consistent for the same file.\n");

    close(fd1);
    close(fd2);
    close(fd3);
    unlink(tmpfile);
    unlink(symlink_path);

    return 0;
}
