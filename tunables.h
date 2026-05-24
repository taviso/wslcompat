#ifndef WSLCOMPAT_TUNABLES_H
#define WSLCOMPAT_TUNABLES_H

#include <stdbool.h>

bool        wslcompat_enabled(const char *name);
const char *wslcompat_tunable_str(const char *key, const char *dflt);
long        wslcompat_tunable_int(const char *key, long dflt);
bool        wslcompat_tunable_bool(const char *key, bool dflt);
bool        wslcompat_tunable_list(const char *key, const char *contains);

// A per-callsite cache for enabled/disabled tunables. By using a macro, each
// shim gets its own private cache, avoiding contention between different
// functions and providing thread-safety (via benign races) without TLS.
#define wslcompat_passthru(name) ({                     \
    static const char *__cached_name;                   \
    static bool        __cached_result;                 \
    const char *__name = (name);                        \
    if (__builtin_expect(__cached_name != __name, 0)) { \
        __cached_result = !wslcompat_enabled(__name);   \
        __cached_name   = __name;                       \
    }                                                   \
    __cached_result;                                    \
})

// Convenience wrapper for shims whose name matches the polyfill.
#define wslcompat_passthru_self() wslcompat_passthru(__func__)

#endif
