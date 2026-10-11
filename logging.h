#ifndef WSLCOMPAT_LOGGING_H
#define WSLCOMPAT_LOGGING_H

enum {
    WSLLOG_ERROR,
    WSLLOG_WARNING,
    WSLLOG_INFO,
    WSLLOG_DEBUG,
    WSLLOG_TRACE,
};

__attribute__((format(printf, 3, 4)))
int wslcompat_debug_log(int level, const char *tag, const char *fmt, ...);

#define wsltrace(fmt, ...) wslcompat_debug_log(WSLLOG_TRACE, __func__, fmt, ## __VA_ARGS__)
#define wsldebug(fmt, ...) wslcompat_debug_log(WSLLOG_DEBUG, __func__, fmt, ## __VA_ARGS__)
#define wslwarn(fmt, ...) wslcompat_debug_log(WSLLOG_WARNING, __func__, fmt, ##__VA_ARGS__)
#define wslinfo(fmt, ...) wslcompat_debug_log(WSLLOG_INFO, __func__, fmt, ##__VA_ARGS__)
#define wslerror(fmt, ...) wslcompat_debug_log(WSLLOG_ERROR, __func__, fmt, ## __VA_ARGS__)

// Shorter variants.
#define wslerr(fmt, ...) wslcompat_debug_log(WSLLOG_ERROR, __func__, fmt, ## __VA_ARGS__)
#define wsldbg(fmt, ...) wslcompat_debug_log(WSLLOG_DEBUG, __func__, fmt, ## __VA_ARGS__)
#define wsllog(fmt, ...) wslcompat_debug_log(WSLLOG_INFO, __func__, fmt, ## __VA_ARGS__)

#endif
