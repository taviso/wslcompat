#pragma GCC optimize("O2")
#define _FORTIFY_SOURCE 2
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include "testutil.h"

int main(int argc, char **argv) {
    STATUS("Testing open(O_TMPFILE) without mode (FORTIFY_SOURCE)");
    
    pid_t pid = fork();
    if (pid == 0) {
        // Use dynamic flags to bypass compile-time static checks
        int flags = O_TMPFILE | O_RDWR;
        if (argc > 1) flags |= O_EXCL;
        
        // This should trigger __chk_fail via __open_2
        open("/tmp", flags);
        
        _exit(0); // If it doesn't abort, exit 0
    }
    
    int status;
    waitpid(pid, &status, 0);
    
    // Natively, glibc aborts the process (SIGABRT or similar)
    CHECK(WIFSIGNALED(status), "open(O_TMPFILE) without mode aborted");
    
    TEST_REPORT();
}
