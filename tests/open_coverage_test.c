#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/xattr.h>
#include <errno.h>
#include <string.h>
#include "testutil.h"

int main(int argc, char **argv) {
    STATUS("Testing open64, creat, and O_PATH combinations");
    
    char tmp[64] = "/tmp/wslcompat_coverage_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd >= 0) close(fd);
    unlink(tmp);
    
    // 1. open64 should route to the shim and record btime
    fd = open64(tmp, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0, "open64 created file");
    
    struct statx stx1 = {0};
    int r1 = statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx1);
    CHECK(r1 == 0 && (stx1.stx_mask & STATX_BTIME), "open64 triggered btime recording");
    
    if (fd >= 0) close(fd);
    unlink(tmp);
    
    // 2. creat should route to the shim and record btime
    fd = creat(tmp, 0644);
    CHECK(fd >= 0, "creat created file");
    
    struct statx stx2 = {0};
    int r2 = statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx2);
    CHECK(r2 == 0 && (stx2.stx_mask & STATX_BTIME), "creat triggered btime recording");
    
    if (fd >= 0) close(fd);
    unlink(tmp);

    // 3. creat64 should route to the shim and record btime
    fd = creat64(tmp, 0644);
    CHECK(fd >= 0, "creat64 created file");
    
    struct statx stx3 = {0};
    int r3 = statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx3);
    CHECK(r3 == 0 && (stx3.stx_mask & STATX_BTIME), "creat64 triggered btime recording");
    
    if (fd >= 0) close(fd);
    
    // 4. O_PATH | O_CREAT should NOT attempt to record btime.
    // Natively, open with O_PATH | O_CREAT ignores O_CREAT. 
    // If the file doesn't exist, it fails with ENOENT.
    unlink(tmp);
    fd = open(tmp, O_PATH | O_CREAT, 0644);
    CHECK(fd < 0 && errno == ENOENT, "O_PATH | O_CREAT on non-existent file fails with ENOENT");
    if (fd >= 0) close(fd);
    
    // If the file DOES exist, O_PATH opens it. Our shim should NOT attempt to
    // add O_EXCL and record btime, which would fail inside the shim.
    fd = creat(tmp, 0644);
    struct statx stx_before = {0};
    int b_ret = statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx_before);
    CHECK(fd >= 0 && b_ret == 0 && (stx_before.stx_mask & STATX_BTIME), "creat setup for O_PATH test");
    if (fd >= 0) close(fd);
    
    fd = open(tmp, O_PATH | O_CREAT, 0644);
    CHECK(fd >= 0, "O_PATH | O_CREAT on existing file succeeds");
    if (fd >= 0) close(fd);
    
    // Now verify the shim didn't try to overwrite the btime!
    fd = open(tmp, O_RDONLY);
    CHECK(fd >= 0, "open for O_PATH verification");
    
    struct statx stx_after = {0};
    int a_ret = statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx_after);
    CHECK(a_ret == 0 && (stx_after.stx_mask & STATX_BTIME) &&
          stx_before.stx_btime.tv_sec == stx_after.stx_btime.tv_sec && 
          stx_before.stx_btime.tv_nsec == stx_after.stx_btime.tv_nsec, 
          "O_PATH | O_CREAT did not interfere with existing btime");
          
    if (fd >= 0) close(fd);
    unlink(tmp);
    
    // 5. Verify the open wrappers don't leak file descriptors
    int initial_fds = wslcompat_test_count_fds();
    for (int i = 0; i < 100; i++) {
        fd = creat(tmp, 0644);
        if (fd >= 0) close(fd);
        
        fd = creat64(tmp, 0644);
        if (fd >= 0) close(fd);
        
        fd = open(tmp, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        
        fd = open64(tmp, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        
        fd = openat(AT_FDCWD, tmp, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        
        fd = openat64(AT_FDCWD, tmp, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        
        fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
        if (fd >= 0) close(fd);
        
        fd = open("/proc/self/mem", O_RDONLY);
        if (fd >= 0) close(fd);
    }
    int final_fds = wslcompat_test_count_fds();
    CHECK(final_fds == initial_fds, "open wrappers did not leak fds (initial=%d, final=%d)", initial_fds, final_fds);

    // 6. Verify O_PATH | O_TMPFILE bypasses our O_TMPFILE shim and falls back to native O_PATH directory open
    // Since native Linux ignores __O_TMPFILE when O_PATH is passed, it should open /tmp as O_PATH.
    fd = open("/tmp", O_PATH | O_TMPFILE, 0644);
    CHECK(fd >= 0, "O_PATH | O_TMPFILE succeeded");
    if (fd >= 0) close(fd);

    unlink(tmp);

    TEST_REPORT();
}
