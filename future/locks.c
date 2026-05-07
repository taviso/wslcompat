#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

#define MAX_LOCKS 1024
#define SHM_NAME "/dev/shm/wslcompat"
#define SHM_MAGIC 0x316c7377
#define SHM_VERSION 1

// There is no kcmp() in wsl1, so we have no way of identifying
// if two file descriptors are the same OFD. The only solution
// we have is to manipulate the state of one fd, and see if it
// matches in the other fd. If it does, it's the same OFD.
//      - We could toggle flock(), but we need that for lock primitives.
//      - We could toggle F_SETFL, e.g. O_APPEND, but could interfere with other users.
//      - File position? Won't work on zero length files, and will interfere with other users.
// We can keep a copy of stat(), and check the dev_t and ino_t match, that's an easy way to
// prove it's not the same OFD, but doesn't mean it is.
//
// - We can record what we think the lock state of all ofd is, can we safely check then?
// - We can dup anything we want, does that help?
struct filelock {
    struct stat     st;
    struct flock    fl;
    pthread_mutex_t mu;
};

struct locktable {
    uint32_t magic;
    uint32_t version;
    pthread_mutex_t mutex;
    struct filelock locks[MAX_LOCKS];
};

static struct locktable *manager = NULL;

static int (*sym_fcntl64)(int fd, int cmd, ...);
static int (*sym_fcntl)(int fd, int cmd, ...);

static struct locktable * init_shm(void)
{
    char tmpfile[] = SHM_NAME ".XXXXXX";
    struct locktable *lt = MAP_FAILED;
    int fd = -1;

    // Try to open an existing shared segment
    if ((fd = open(SHM_NAME, O_RDWR, 0)) < 0) {
        // If the file doesn't exist we can create it, otherwise...
        if (errno != ENOENT)
            goto cleanup;

        // Create a new tmpfile while we prepare it.
        if ((fd = mkstemp(tmpfile)) < 0)
            goto cleanup;

        // We're the creator, so initialize it.
        if (ftruncate(fd, sizeof(struct locktable)) < 0) {
            goto cleanup;
        }
    }

    // Attempt to map the locktable.
    lt = mmap(NULL, sizeof(*lt), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (lt == MAP_FAILED)
        goto cleanup;

    // We must have just created this, lets initialize it.
    if (lt->magic != SHM_MAGIC) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&lt->mutex, &attr);
        lt->magic = SHM_MAGIC;
        lt->version = SHM_VERSION;

        // Try to atomically install this as the global locktable.
        if (link(tmpfile, SHM_NAME) != 0) {
            if (errno == EEXIST)
                goto retry;
            goto cleanup;
        }

        // Remove the old name
        unlink(tmpfile);
    }

    // No longer need the file descriptor.
    close(fd);

    // Successfully mapped.
    return lt;

  retry:
    lt = init_shm();

  cleanup:
    if (fd != -1)
        close(fd);
    if (lt != MAP_FAILED)
        munmap(lt, sizeof(*lt));
    if (strstr(tmpfile, ".XXXXXX") != NULL)
        unlink(tmpfile);
    return lt;
}

// We have taken over flock and we use it to know if a lock exists.
static int file_locked(int fd)
{
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        return 0;
    return 1;
}

// Prerequisites
//
// - Nobody can call flock() outside of this wrapper - we have a monopoly on it.


// How to check if two fd are the same ofd
//
//  Lock holder is stuck in this:
//
//  flock(fd);
//  pthread_mutex_lock(&lt->mutex);
//  pthread_mutex_unlock(&lt->mutex);
//
//  The reason is we mess with flags for locked files and dont promies they're right
//  the table is unlocked.
//
//  How do we handle races?
//
int posix_process_lock(int fd, int op, struct flock *lock)
{
    pthread_mutex_lock(&lt->mutex);

    // F_SETLK
    // Acquire or release a lock
        // flock nb this fd
            // EWOULDBLOCK -> there is already a lock on this file
                // Is this lock range compatible with existing locks?
                    // stat() -> search table -> for matching stats.
                        // found a match, how do we check they're the same?
                            // same -> compare ranges, if okay allow, lock okay
                // Is the pid a match?
                    // posix locks are process scoped, no match, you can have the lock.
                        // add to the list
            // Success -> you can have this lock
                // Add to table
                // lock granted
    pthread_mutex_unlock(&lt->mutex);
    return 0;
}

int linux_ofd_lock(int fd, int op, struct flock *lock);
{
    pthread_mutex_lock(&lt->mutex);
    pthread_mutex_unlock(&lt->mutex);
    return 0;
}

int bsd_file_lock(int fd, int op, int flags);
{
    pthread_mutex_lock(&lt->mutex);
    pthread_mutex_unlock(&lt->mutex);
    return 0;
}

// wrapper

//int main(int argc, char **argv)
//{
//    printf("hello %p", init_shm());
//}
