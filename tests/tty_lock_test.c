#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pty.h>
#include <errno.h>

#include "testutil.h"

#include "testutil.h"

int main() {
    int p[2];
    if (pipe(p) < 0) return 1;
    int master, slave;
    char name[64] = {0};
    if (openpty(&master, &slave, name, NULL, NULL) < 0) {
        return 1;
    }

    struct termios t;
    if (ioctl(slave, TCGETS, &t) < 0) {
        return 1;
    }
    
    // trigger wslcompat_tty_install
    if (ioctl(slave, TCSETS, &t) < 0) {
        return 1;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        return 1;
    }
    
    if (pid == 0) {
        // Child
        close(p[1]);
        char c;
        // Wait for parent to signal it holds the lock
        if (read(p[0], &c, 1) != 1) exit(1);
        close(p[0]);
        
        if (c == 'q') {
            // Parent couldn't acquire lock (e.g. WSL1 EOPNOTSUPP). Nothing to test.
            exit(0);
        }
        
        // Open a new fd to the same slave to simulate another process opening the tty.
        int fd = open(name, O_RDWR);
        if (fd < 0) {
            exit(1);
        }
        
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;
        
        // This should FAIL with EWOULDBLOCK or EAGAIN because parent holds the lock.
        if (fcntl(fd, F_SETLK, &fl) == 0) {
            exit(1); // Child successfully locked tty despite parent holding lock! (bug)
        }
        
        exit(0);
    } else {
        // Parent
        close(p[0]);
        
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;
        
        int r = fcntl(slave, F_SETLK, &fl);
        
        if (wslcompat_test_on_wsl()) {
            // On WSL1, flock() on a PTY natively returns EOPNOTSUPP.
            // Before the fcntl.c fix, wslcompat falsely returned 0 (success) because it
            // locked the private O_PATH tmpfile. After the fix, it correctly routes to
            // the real backing fd and returns EOPNOTSUPP.
            CHECK(r == -1 && errno == EOPNOTSUPP, "TTY locking should correctly fail with EOPNOTSUPP on WSL1");
            
            // Signal child to exit early
            if (write(p[1], "q", 1) != 1) return 1;
            close(p[1]);
            waitpid(pid, NULL, 0);
            TEST_REPORT();
            return 0;
        } else {
            // Native Linux
            CHECK(r == 0, "TTY locking should succeed natively on Linux");
            
            // Signal child that lock is held
            if (write(p[1], "x", 1) != 1) return 1;
            close(p[1]);
            
            int status;
            waitpid(pid, &status, 0);
            
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child should fail to acquire lock");
            
            TEST_REPORT();
            return 0;
        }
    }
}
