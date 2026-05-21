#ifndef WSLCOMPAT_TUNABLES_H
#define WSLCOMPAT_TUNABLES_H

#include <stdbool.h>

bool        wslcompat_enabled(const char *name);
const char *wslcompat_tunable_str(const char *key, const char *dflt);
long        wslcompat_tunable_int(const char *key, long dflt);
bool        wslcompat_tunable_bool(const char *key, bool dflt);
bool        wslcompat_tunable_list(const char *key, const char *contains);

// A cached check for enabled/disabled tunables.
static inline bool wslcompat_passthru(const char *name)
{
    static const char *cached_name;
    static bool        cached_result;

    if (__builtin_expect(cached_name != name, 0)) {
        cached_result = !wslcompat_enabled(name);
        cached_name   = name;
    }
    return cached_result;
}

#endif
