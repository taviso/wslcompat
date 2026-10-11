#ifndef WSLCOMPAT_SHIM_H
#define WSLCOMPAT_SHIM_H

#include <errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <dlfcn.h>

// Number of elements in a fixed-size array.
#ifndef _countof
#define _countof(arr)                                                          \
    (sizeof(arr) / sizeof((arr)[0])                                            \
     + sizeof(struct { int:-!!__builtin_types_compatible_p(                    \
         typeof(arr), typeof(&(arr)[0])); }))
#endif

// Optimization barrier that hides a pointer from GCC's __nonnull analysis.
#define HIDE_NONNULL(p) asm volatile ("" : "+r" (p))

// Weak ref to glibc's FORTIFY abort routine.
extern void __chk_fail(void) __attribute__((noreturn, weak));

static inline void __attribute__((noreturn)) wslcompat_chk_fail(void)
{
    if (__chk_fail)
        __chk_fail();
    __builtin_trap();
}

typedef void (*shim_init_fn)(void);

// This inline macro handles verifying symbols are resolved before calling
// them, which can happen with complex constructor chains.
static inline bool _shim_resolved(shim_init_fn init_fn, const void **sym_ptr)
{
    if (__builtin_expect(*sym_ptr == NULL, false)) {
        init_fn();
        return *sym_ptr != NULL;
    }
    return true;
}

// This wraps a call to the next symbol (i.e. RTLD_NEXT) with a guard check to
// ensure the symbol is resolved.
// It returns a typed -1 on failure. Variadic forwarding works because the
// preprocessor splices __VA_ARGS__ directly into the call.
#define sym_next(func, ...)                                                     \
    (_shim_resolved(_shim_init, (const void **)&sym_##func)                     \
        ? sym_##func(__VA_ARGS__)                                               \
        : (errno = ENOSYS, (__typeof__(sym_##func(__VA_ARGS__)))-1))

// SHIM_INIT(name [, name...]) declares static function pointers
// sym_<name> whose types are derived from libc's prototypes (so the
// relevant libc headers must be included first), and emits a static
// constructor `init` that dlsym's them from RTLD_NEXT.
#define _SHIM_DECLARE(name)                                                     \
    static __typeof__(name) *sym_##name;

#define _SHIM_DLSYM(name)                                                       \
    sym_##name = dlsym(RTLD_NEXT, #name);

#define _SHIM_EXPAND(...) __VA_ARGS__

#define _SHIM_FE_1(M, x)       M(x)
#define _SHIM_FE_2(M, x, ...)  M(x) _SHIM_FE_1(M, __VA_ARGS__)
#define _SHIM_FE_3(M, x, ...)  M(x) _SHIM_FE_2(M, __VA_ARGS__)
#define _SHIM_FE_4(M, x, ...)  M(x) _SHIM_FE_3(M, __VA_ARGS__)
#define _SHIM_FE_5(M, x, ...)  M(x) _SHIM_FE_4(M, __VA_ARGS__)
#define _SHIM_FE_6(M, x, ...)  M(x) _SHIM_FE_5(M, __VA_ARGS__)
#define _SHIM_FE_7(M, x, ...)  M(x) _SHIM_FE_6(M, __VA_ARGS__)
#define _SHIM_FE_8(M, x, ...)  M(x) _SHIM_FE_7(M, __VA_ARGS__)
#define _SHIM_FE_9(M, x, ...)  M(x) _SHIM_FE_8(M, __VA_ARGS__)
#define _SHIM_FE_10(M, x, ...) M(x) _SHIM_FE_9(M, __VA_ARGS__)
#define _SHIM_FE_11(M, x, ...) M(x) _SHIM_FE_10(M, __VA_ARGS__)
#define _SHIM_FE_12(M, x, ...) M(x) _SHIM_FE_11(M, __VA_ARGS__)
#define _SHIM_FE_13(M, x, ...) M(x) _SHIM_FE_12(M, __VA_ARGS__)
#define _SHIM_FE_14(M, x, ...) M(x) _SHIM_FE_13(M, __VA_ARGS__)
#define _SHIM_FE_15(M, x, ...) M(x) _SHIM_FE_14(M, __VA_ARGS__)
#define _SHIM_FE_16(M, x, ...) M(x) _SHIM_FE_15(M, __VA_ARGS__)
#define _SHIM_FE_17(M, x, ...) M(x) _SHIM_FE_16(M, __VA_ARGS__)
#define _SHIM_FE_18(M, x, ...) M(x) _SHIM_FE_17(M, __VA_ARGS__)
#define _SHIM_FE_19(M, x, ...) M(x) _SHIM_FE_18(M, __VA_ARGS__)
#define _SHIM_FE_20(M, x, ...) M(x) _SHIM_FE_19(M, __VA_ARGS__)

#define _SHIM_FE_PICK(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,                   \
                      _13,_14,_15,_16,_17,_18,_19,_20, N, ...) N
#define _SHIM_FOR_EACH(M, ...)                                                  \
    _SHIM_FE_PICK(__VA_ARGS__,                                                  \
                  _SHIM_FE_20, _SHIM_FE_19, _SHIM_FE_18, _SHIM_FE_17,           \
                  _SHIM_FE_16, _SHIM_FE_15, _SHIM_FE_14, _SHIM_FE_13,           \
                  _SHIM_FE_12, _SHIM_FE_11, _SHIM_FE_10, _SHIM_FE_9,            \
                  _SHIM_FE_8,  _SHIM_FE_7,  _SHIM_FE_6,  _SHIM_FE_5,            \
                  _SHIM_FE_4,  _SHIM_FE_3,  _SHIM_FE_2,  _SHIM_FE_1)            \
        (M, __VA_ARGS__)

#define SHIM_INIT(...)                                                          \
    _SHIM_FOR_EACH(_SHIM_DECLARE, __VA_ARGS__)                                  \
    static void __attribute__((constructor)) _shim_init(void)                   \
    {                                                                           \
        _SHIM_FOR_EACH(_SHIM_DLSYM, __VA_ARGS__)                                \
        __asm__ __volatile__ ("");                                              \
    }

// Create a symbol alias that avoids any symbol resolution machinery. Call
// __wslcompat_syscall to always our implementation.
#define SHIM_ALIAS(name) \
    extern __typeof__(name) __wslcompat_##name __attribute__((alias(#name)))

#endif
