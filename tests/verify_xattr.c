#include <sys/xattr.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    char tmpfile[] = "/tmp/wslcompat.XXXXXX";
    int fd = open(tmpfile, O_RDWR | O_CREAT, 0666);
    const char *val = "hello";
    char buf[64] = {0};

    printf("Setting xattr...\n");
    if (fsetxattr(fd, "user.wslcompat", val, strlen(val), 0) == -1) {
        perror("fsetxattr");
        return 1;
    } else {
        printf("SUCCESS!\n");
        fgetxattr(fd, "user.wslcompat", buf, sizeof(buf));
        printf("Read back: %s\n", buf);
    }

    close(fd);
    unlink(tmpfile);
    return strcmp(buf, val);
}
