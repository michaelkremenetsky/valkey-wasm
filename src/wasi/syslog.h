/* wasi-sysroot has no <syslog.h>. Valkey's logging goes to its own logfile/
 * stdout path; syslog is only used when daemonized (never here). Provide the
 * symbols as no-ops so the ~64 TUs that transitively include it compile.
 * On the -I path (build.sh), so `#include <syslog.h>` resolves here. */
#ifndef VALKEY_WASM_SYSLOG_H
#define VALKEY_WASM_SYSLOG_H
#include <stdarg.h>

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10

#define LOG_KERN   (0 << 3)
#define LOG_USER   (1 << 3)
#define LOG_LOCAL0 (16 << 3)
#define LOG_LOCAL1 (17 << 3)
#define LOG_LOCAL2 (18 << 3)
#define LOG_LOCAL3 (19 << 3)
#define LOG_LOCAL4 (20 << 3)
#define LOG_LOCAL5 (21 << 3)
#define LOG_LOCAL6 (22 << 3)
#define LOG_LOCAL7 (23 << 3)

static inline void openlog(const char *ident, int option, int facility) {
    (void)ident; (void)option; (void)facility;
}
static inline void syslog(int priority, const char *format, ...) {
    (void)priority; (void)format;
}
static inline void closelog(void) {}
static inline int setlogmask(int mask) { (void)mask; return 0; }

#endif
