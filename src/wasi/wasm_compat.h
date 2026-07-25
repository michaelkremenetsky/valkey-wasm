/* Force-included ahead of every Valkey TU (build.sh -include).
 * Neutralizes host facilities the wasm32-wasip1 sandbox lacks, at the macro
 * level, so the vendored source stays otherwise untouched. Networking and the
 * event-loop poll are provided for real by sock_shim.c / ae_wasi.c over host
 * imports; everything here is the "can't exist in wasm" set: fork-based
 * persistence, background threads, rlimits, crash backtraces. */
#ifndef VALKEY_WASM_COMPAT_H
#define VALKEY_WASM_COMPAT_H

/* This header is force-included (-include) ahead of each TU's own headers,
 * before Valkey's fmacros.h sets it. Establish the feature level now so the
 * <signal.h> we pull in below exposes sigset_t / sigemptyset etc. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef VALKEY_WASM

/* No fork(): BGSAVE/AOF-rewrite child spawning. Persistence is disabled at
 * runtime (save "" / appendonly no), so these paths are never *meant* to run;
 * make the symbol resolve to a failing stub (stubs.c) rather than pulling in
 * wasi-libc's missing fork. */
#define fork() (-1)
#define wait3(a, b, c) (-1)
#define waitpid(a, b, c) (-1)

/* Background I/O threads and io-threads are compiled out; bio jobs run inline
 * (see stubs.c bioInit override guard). io_threads_num defaults to 1. */

/* rlimits: no-op (stubs.c provides the symbols too; the macro keeps callers
 * that check the return value happy). */
#define setrlimit(a, b) (0)

/* ---- POSIX bits wasi-libc stubs out (everything below is under an
 *      `#ifdef __wasilibc_unmodified_upstream` in the sysroot headers, i.e.
 *      NOT provided). Valkey's crash handler + signal setup + fd-limit code
 *      reference them; the handlers are never actually delivered a signal in
 *      the sandbox, so these definitions exist only to compile those paths.
 *      Force-included ahead of the sysroot headers, so ours win and the
 *      disabled sysroot copies are inert. ---- */

/* rlimit: <sys/resource.h> provides rusage/getrusage on WASI but not the
 * rlimit family. getrlimit/setrlimit are stubbed (stubs.c / macro above). */
typedef unsigned long long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
#ifndef RLIMIT_NOFILE
#define RLIMIT_NOFILE 7
#endif
#ifndef RLIM_INFINITY
#define RLIM_INFINITY (~0ULL)
#endif
int getrlimit(int resource, struct rlimit *rlim);

/* signals: wasi-emulated-signal's <bits/signal.h> gives the SIG* constants +
 * signal()/raise(), but wasi-libc stubs out the whole POSIX signal-set /
 * sigaction / sigmask API and siginfo_t. None of it can ever deliver a signal
 * in the sandbox, so provide the types plus no-op operations: installing a
 * handler succeeds and simply never fires. sigset_t itself is behind the same
 * disabled guard in <signal.h>, so pull it directly from bits/alltypes.h. */
#define __NEED_sigset_t
#define __NEED_pid_t
#define __NEED_uid_t
#include <bits/alltypes.h>
#include <signal.h>   /* SIG* constants, signal(), raise(), SIG_BLOCK */

#ifndef SA_SIGINFO
typedef struct {
    int si_signo, si_errno, si_code;
    int si_pid;
    unsigned si_uid;
    void *si_addr;
} siginfo_t;
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};
#define SA_SIGINFO   0x00000004
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTART   0x10000000
#define SA_ONSTACK   0x08000000
#ifndef SI_USER
#define SI_USER 0
#endif

static inline int sigemptyset(sigset_t *s) { if (s) { sigset_t z = {0}; *s = z; } return 0; }
static inline int sigfillset(sigset_t *s) { (void)s; return 0; }
static inline int sigaddset(sigset_t *s, int n) { (void)s; (void)n; return 0; }
static inline int sigdelset(sigset_t *s, int n) { (void)s; (void)n; return 0; }
static inline int sigismember(const sigset_t *s, int n) { (void)s; (void)n; return 0; }
static inline int sigprocmask(int how, const sigset_t *s, sigset_t *o) { (void)how; (void)s; (void)o; return 0; }
static inline int pthread_sigmask(int how, const sigset_t *s, sigset_t *o) { (void)how; (void)s; (void)o; return 0; }
static inline int sigaction(int signum, const struct sigaction *act, struct sigaction *old) {
    (void)signum; (void)act; (void)old; return 0;
}
static inline int kill(pid_t pid, int sig) { (void)pid; (void)sig; return 0; }
#endif /* SA_SIGINFO */

/* interval timers: the watchdog (debug.c) and the Lua time-limit (eval.c) arm
 * setitimer(ITIMER_REAL) to raise SIGALRM. No signal is ever delivered here, so
 * arming is a no-op success. struct itimerval needs struct timeval. */
#include <sys/time.h>
#ifndef ITIMER_REAL
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
struct itimerval { struct timeval it_interval, it_value; };
static inline int setitimer(int which, const struct itimerval *nv, struct itimerval *ov) {
    (void)which; (void)nv; (void)ov; return 0;
}
static inline int getitimer(int which, struct itimerval *cv) { (void)which; (void)cv; return 0; }
#endif

/* Prototypes for the libc functions wasi-libc leaves undeclared but that we
 * define in stubs.c. Without a visible prototype the callers use an implicit
 * `int f()` signature, which mismatches the real definition and makes wasm-ld
 * emit a trapping signature-adapter stub. Declaring them here (force-included
 * ahead of every TU) keeps the call and the definition in the same signature. */
void tzset(void);
int  flock(int fd, int op);
int  mkostemp(char *tmpl, int flags);
int  pipe(int fd[2]);
int  execve(const char *path, char *const argv[], char *const envp[]);
unsigned umask(unsigned mask);

/* errno codes wasi-libc omits, referenced only in server.c's listen-failure
 * error classification (the value is irrelevant, only that the name exists). */
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 240
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT 241
#endif

/* socket-layer constants: wasi's <sys/socket.h> keeps most of the BSD option/
 * address-family/TCP names in a disabled upstream branch. anet.c passes them to
 * setsockopt()/socket(), which are no-op shims here (sock_shim.c) - the JS
 * bridge owns the real socket options - so only the names need to exist.
 * AF_INET, SOCK_STREAM and SOL_SOCKET already come from the sysroot; guard the
 * rest with #ifndef so we never clash with what is genuinely provided. */
#include <sys/socket.h>
#ifndef SO_ERROR
#define SO_ERROR 4
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 9
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 66
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 67
#endif
#ifndef AF_LOCAL
#define AF_LOCAL 1
#endif
#ifndef AF_UNIX
#define AF_UNIX AF_LOCAL
#endif
#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 26
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
#ifndef TCP_KEEPALIVE
#define TCP_KEEPALIVE 0x10
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 4
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 5
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 6
#endif

/* dladdr / Dl_info: <dlfcn.h> stubs these out on WASI. The crash handler uses
 * them to prettify a backtrace; dladdr() just reports "not found" (stubs.c). */
#ifndef VALKEY_WASM_HAVE_DLINFO
#define VALKEY_WASM_HAVE_DLINFO
typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;
int dladdr(const void *addr, Dl_info *info);
#endif

#endif /* VALKEY_WASM */
#endif
