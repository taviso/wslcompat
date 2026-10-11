#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "testutil.h"

#define NUM_THREADS 8
static struct timespec g_deadline;

static int past_deadline(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec != g_deadline.tv_sec)
        return now.tv_sec > g_deadline.tv_sec;
    return now.tv_nsec >= g_deadline.tv_nsec;
}

static void *worker(void *arg)
{
    (void)arg;
    uintptr_t failures = 0;
    
    unsigned int seed = time(NULL) ^ pthread_self();
    
    while (!past_deadline()) {
        int fd = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
        if (fd < 0) continue;

        for (int i = 0; i < 20; i++) {
            int target = fd + 1 + (rand_r(&seed) % 32);
            close(target);
            int d = dup2(fd, target);
            if (d >= 0) close(d);
        }
        close(fd);
    }
    return (void *)failures;
}

int main(int argc, char **argv)
{
    double total_seconds = 0.5;
    if (argc > 1) {
        total_seconds = atof(argv[1]);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &g_deadline);
    long whole = (long) total_seconds;
    long nsec  = (long) ((total_seconds - whole) * 1e9);
    g_deadline.tv_sec  += whole;
    g_deadline.tv_nsec += nsec;
    if (g_deadline.tv_nsec >= 1000000000) {
        g_deadline.tv_sec  += 1;
        g_deadline.tv_nsec -= 1000000000;
    }

    pthread_t threads[NUM_THREADS];
    for (long i = 0; i < NUM_THREADS; i++) {
        CHECK(pthread_create(&threads[i], NULL, worker, (void *)i) == 0,
              "pthread_create thread %ld", i);
    }

    uintptr_t total_fail = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        void *r;
        pthread_join(threads[i], &r);
        total_fail += (uintptr_t)r;
    }

    CHECK(total_fail == 0, "No data races or crashes during internal fd relocation");
    
    TEST_REPORT();
}
