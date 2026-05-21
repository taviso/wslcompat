#define _GNU_SOURCE
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <search.h>

#include "tunables.h"
#include "logging.h"

#define MAX_OPTIONS     32
#define KEY_BUF         64
#define VAL_BUF         512
#define LIST_BUF        1024

static const char kPrefix[] = "user.wslcompat.";

static struct opt {
    char key[KEY_BUF];
    char val[VAL_BUF];
} options[MAX_OPTIONS];

_Static_assert(offsetof(struct opt, key) == 0,
               "optcmp reads from the start of struct opt; key must be first");

static size_t options_count;

static int optcmp(const void *a, const void *b)
{
    return strcmp(a, b);
}

static int tunables_add_option(const char *key, const char *val)
{
    struct opt entry = {0};

    if (options_count >= MAX_OPTIONS)
        return -1;
    if (strncmp(key, kPrefix, sizeof(kPrefix) - 1) != 0)
        return -1;

    // Skip over user.wslcompat prefix.
    key += sizeof(kPrefix) - 1;

    snprintf(entry.key, sizeof(entry.key), "%s", key);
    snprintf(entry.val, sizeof(entry.val), "%s", val);
    lsearch(&entry, options, &options_count, sizeof(*options), optcmp);
    return 0;
}

static void __attribute__((constructor)) init(void)
{
    char    list[LIST_BUF] = {0};
    char    val[VAL_BUF];
    ssize_t n;
    ssize_t size;
    int     fd;

    if ((fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC)) < 0)
        return;

    if ((n = flistxattr(fd, list, sizeof(list) - 1)) <= 0)
        goto cleanup;

    for (char * cur = list; cur - list < n; cur += strlen(cur) + 1) {
        // Check if this is one of ours.
        if (strncmp(cur, kPrefix, sizeof(kPrefix) - 1) != 0)
            continue;

        // Fetch the value.
        if ((size = fgetxattr(fd, cur, val, sizeof(val) - 1)) < 0)
            continue;

        // fgetxattr returns raw bytes without a terminator.
        val[size] = '\0';

        // Record the list.
        if (tunables_add_option(cur, val) < 0)
            break;
    }

  cleanup:
    close(fd);
}

const char *wslcompat_tunable_str(const char *key, const char *dflt)
{
    struct opt *r;

    if ((r = lfind(key, options, &options_count, sizeof(*options), optcmp)))
        return r->val;
    return dflt;
}

// A list is a comma seperated list of values.
bool wslcompat_tunable_list(const char *key, const char *contains)
{
    const char *s;

    if ((s = wslcompat_tunable_str(key, NULL)) == NULL)
        return false;

    // Scan the list for a matching entry.
    for (const char *p = s; *p; s = ++p) {
        // Point at next entry.
        p = strchrnul(s, ',');

        // Check if match
        if (strncmp(s, contains, p - s) == 0)
            return true;
    };

    return false;
}

// Test if a feature is enabled/disabled
bool wslcompat_enabled(const char *name)
{
    // Is this feature is listed in disabled?
    if (wslcompat_tunable_list("disabled", name))
        return false;
    // Does enabled exist?
    if (wslcompat_tunable_str("enabled", NULL) == NULL)
        return true;
    // Is this key listed in enabled?
    if (wslcompat_tunable_list("enabled", name))
        return true;
    // There is an enabled list, and this feature isnt it.
    return false;
}

long wslcompat_tunable_int(const char *key, long dflt)
{
    const char *s;
    if ((s = wslcompat_tunable_str(key, NULL)) == NULL)
        return dflt;
    return strtol(s, NULL, 0);
}

bool wslcompat_tunable_bool(const char *key, bool dflt)
{
    const char *s;
    if ((s = wslcompat_tunable_str(key, NULL)) == NULL)
        return dflt;
    if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "on") || !strcmp(s, "yes"))
        return true;
    if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "off") || !strcmp(s, "no"))
        return false;
    return dflt;
}
