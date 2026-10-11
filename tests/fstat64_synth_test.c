/* fstat64() on a wrapped tty must report the same backing stat as
 * fstat(). The fstat64 shim casts struct stat64 * to struct stat *
 * and forwards to fstat(); if that cast were wrong (or fstat64 were
 * not interposed at all), the wrapped fd would surface the O_TMPFILE
 * backing inode (S_IFREG) instead of the real tty (S_ISCHR). */
#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "testutil.h"

static void open_pty(int *master_fd, int *slave_fd)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s == -1) err(EXIT_FAILURE, "open slave");
    *master_fd = m;
    *slave_fd  = s;
}

static void set_raw(int fd)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    STATUS("--- fstat64 matches fstat on a wrapped tty ---");
    int m, s;
    open_pty(&m, &s);
    set_raw(s); /* installs the synth wrap when the shim is loaded */

    struct stat   st;
    struct stat64 st64;

    CHECK(fstat(s, &st) == 0, "fstat on wrapped fd -> ok");
    CHECK(fstat64(s, &st64) == 0, "fstat64 on wrapped fd -> ok");

    /* The cast is only correct if every field matches; a layout
     * mismatch or an uninterposed fstat64 would diverge here. */
    CHECK(st64.st_mode == st.st_mode,
          "st_mode matches (0%o vs 0%o)", st64.st_mode, st.st_mode);
    CHECK(st64.st_rdev == st.st_rdev,
          "st_rdev matches (%lu vs %lu)",
          (unsigned long)st64.st_rdev, (unsigned long)st.st_rdev);
    CHECK(st64.st_ino == st.st_ino,
          "st_ino matches (%lu vs %lu)",
          (unsigned long)st64.st_ino, (unsigned long)st.st_ino);
    CHECK(st64.st_size == st.st_size,
          "st_size matches (%ld vs %ld)",
          (long)st64.st_size, (long)st.st_size);

    /* Both must see the character device, never the O_TMPFILE. */
    CHECK(S_ISCHR(st.st_mode), "fstat -> character device (0%o)", st.st_mode);
    CHECK(S_ISCHR(st64.st_mode), "fstat64 -> character device (0%o)", st64.st_mode);

    close(m);
    close(s);

    TEST_REPORT();
}
