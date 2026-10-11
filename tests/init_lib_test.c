#include <stdio.h>
#include <unistd.h>

/*
 * This library has a constructor that calls read().
 * If libwslcompat.so (which intercepts read()) is not initialized first,
 * this will crash because sym_read will be NULL.
 *
 * The purpose is to verify -z initfirst is working.
 */

static void __attribute__((constructor)) init(void) {
    printf("constructor in test library calling read()...\n");
    read(0, NULL, 0);
    printf("constructor in test library finished.\n");
}
