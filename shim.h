#ifndef WSLCOMPAT_SHIM_H
#define WSLCOMPAT_SHIM_H

#include <stdbool.h>
#include <dlfcn.h>

typedef int (*shim_init_fn)(void);

// This inline macro handles verifying symbols are resolved before calling
// them, which can happen with complex constructor chains.
static inline bool _shim_resolved(shim_init_fn init_fn, const void *sym)
{
    if (__builtin_expect(sym == NULL, false)) {
        return init_fn() == 0;
    }
    return true;
}

// This wraps a call to the next symbol (i.e. RTLD_NEXT) with a guard check to
// ensure the symbol is resolved.
// It returns a typed -1 on failure. Variadic forwarding works because the
// preprocessor splices __VA_ARGS__ directly into the call.
#define sym_next(func, ...)                                                     \
    (_shim_resolved(_shim_init, sym_##func)                                     \
        ? sym_##func(__VA_ARGS__)                                               \
        : (__typeof__(sym_##func(__VA_ARGS__)))-1)

// SHIM_INIT(name [, name...]) declares static function pointers
// sym_<name> whose types are derived from libc's prototypes (so the
// relevant libc headers must be included first), and emits a static
// constructor `init` that dlsym's them from RTLD_NEXT. Returns 0 on
// success, -1 if any dlsym fails.
#define _SHIM_DECLARE(name)                                                     \
    static __typeof__(name) *sym_##name;

#define _SHIM_DLSYM(name)                                                       \
    if ((sym_##name = dlsym(RTLD_NEXT, #name)) == NULL) return -1;

#define _SHIM_FE_1(M, x)      M(x)
#define _SHIM_FE_2(M, x, ...) M(x) _SHIM_FE_1(M, __VA_ARGS__)
#define _SHIM_FE_3(M, x, ...) M(x) _SHIM_FE_2(M, __VA_ARGS__)
#define _SHIM_FE_4(M, x, ...) M(x) _SHIM_FE_3(M, __VA_ARGS__)
#define _SHIM_FE_PICK(_1,_2,_3,_4, N, ...) N
#define _SHIM_FOR_EACH(M, ...)                                                  \
    _SHIM_FE_PICK(__VA_ARGS__, _SHIM_FE_4, _SHIM_FE_3, _SHIM_FE_2, _SHIM_FE_1)  \
        (M, __VA_ARGS__)

#define SHIM_INIT(...)                                                          \
    _SHIM_FOR_EACH(_SHIM_DECLARE, __VA_ARGS__)                                  \
    static int __attribute__((constructor)) _shim_init(void)                    \
    {                                                                           \
        _SHIM_FOR_EACH(_SHIM_DLSYM, __VA_ARGS__)                                \
        return 0;                                                               \
    }

#endif
