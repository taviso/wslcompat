#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include "testutil.h"

extern char **environ;

int main(int argc, char **argv) {
    if (argc == 1) {
        // Stage 1: Set xattrs on our own executable and re-exec.
        // This ensures tunables.c reads the new xattrs during its constructor.
        // We use argv[0] instead of /proc/self/exe because under valgrind,
        // /proc/self/exe points to valgrind itself.
        CHECK(setxattr(argv[0], "user.wslcompat.debug", "3", 1, 0) == 0, "set debug=3");
        
        const char *logfile = "/tmp/wslcompat_test.log";
        CHECK(setxattr(argv[0], "user.wslcompat.logfile", logfile, strlen(logfile), 0) == 0, "set logfile");
        
        unlink(logfile); // ensure clean state
        
        char *args[] = { argv[0], "stage2", NULL };
        execve(argv[0], args, environ);
        perror("execve");
        return 1;
    }
    
    // Stage 2: We are running with xattrs applied.
    int (*debug_log)(int, const char *, const char *, ...) = dlsym(RTLD_DEFAULT, "wslcompat_debug_log");
    
    if (debug_log) {
        char huge[1024];
        memset(huge, 'A', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';
        
        // This log should be truncated but successfully written.
        int r = debug_log(0, "TEST", "Huge string: %s", huge);
        CHECK(r > 0, "debug_log handled the huge string without failing");
        
        // Verify the log file was created and contains the truncated string.
        int logfd = open("/tmp/wslcompat_test.log", O_RDONLY);
        CHECK(logfd >= 0, "log file was created");
        if (logfd >= 0) {
            char logbuf[1024] = {0};
            read(logfd, logbuf, sizeof(logbuf) - 1);
            CHECK(strstr(logbuf, "Huge string: AAAA") != NULL, "log contains truncated string");
            
            // Check that it ends with a newline despite truncation.
            char *last_newline = strrchr(logbuf, '\n');
            CHECK(last_newline != NULL, "log contains newline");
            if (last_newline) {
                CHECK(*(last_newline + 1) == '\0', "newline is at the very end of the truncated log");
            }
            
            close(logfd);
        }
    } else {
        STATUS("wslcompat_debug_log not found (running natively without LD_PRELOAD)");
    }
    
    unlink("/tmp/wslcompat_test.log");
    
    TEST_REPORT();
    return 0;
}
