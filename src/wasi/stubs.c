/* Stubs for host facilities absent on wasm32-wasip1. Persistence/threads are
 * off at runtime, so these paths are configured never to run in earnest; the
 * stubs satisfy the linker and do the minimal correct thing when a code path
 * does reach them (e.g. bio jobs execute inline instead of on a thread). */
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>

/* ---- process / fork family (persistence + BGSAVE never run) ---- */
/* fork()/wait* are macro'd to failing constants in wasm_compat.h; provide the
 * symbol too for any indirect reference. */
int __wasm_fork(void) { errno = ENOSYS; return -1; }

/* ---- rlimits (no sandbox limits to raise; getrusage is provided by
 *      wasi-emulated-process-clocks, so we must NOT redefine it here). struct
 *      rlimit + the getrlimit prototype come from wasm_compat.h. Report a
 *      generous fd limit so adjustOpenFilesLimit() is satisfied. ---- */
int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource;
    if (rlim) { rlim->rlim_cur = 1ull << 20; rlim->rlim_max = 1ull << 20; }
    return 0;
}

/* ---- crash backtrace / dynamic-symbol lookup (no unwinder or loader) ---- */
int backtrace(void **buffer, int size) { (void)buffer; (void)size; return 0; }
char **backtrace_symbols(void *const *buffer, int size) { (void)buffer; (void)size; return 0; }
void backtrace_symbols_fd(void *const *buffer, int size, int fd) { (void)buffer; (void)size; (void)fd; }
int dladdr(const void *addr, Dl_info *info) { (void)addr; (void)info; return 0; }
/* sigemptyset/sigaction/kill/... are inline no-ops in wasm_compat.h. */

/* ---- TLS: tls.c is excluded from the build (it crashes this wasi-sdk's
 *      codegen even in its no-OpenSSL form, and TLS is not used). Provide the
 *      one symbol connection.c needs; C_ERR reports "TLS not built in". ---- */
int RedisRegisterConnectionTypeTLS(void) { return -1; /* C_ERR */ }
/* config.c calls this on every tls-* config apply, even with TLS off; no-op. */
void tlsResetCertInfo(void) {}

/* ---- RDMA (new in 9.x): rdma.c is excluded (its non-Linux stub still crashes
 *      this wasi-sdk's codegen), and RDMA is not used. connection.c calls this
 *      at startup; C_ERR simply reports "RDMA supported on Linux only". ---- */
int RegisterConnectionTypeRdma(void) { return -1; /* C_ERR */ }

/* ==== Background I/O (bio) — pthread-free: every job runs INLINE ============
 * bio.c is excluded from the build (it needs pthreads). Valkey's bio API is a
 * deferral mechanism for close()/fsync()/lazy-free so the main thread never
 * blocks on disk. With no threads we just do the work immediately on submit;
 * semantically identical, only synchronous. Signatures match bio.h. */
typedef void lazy_free_fn(void *args[]);

void bioInit(void) {}
unsigned long bioPendingJobsOfType(int type) { (void)type; return 0; }
void bioDrainWorker(int job_type) { (void)job_type; }
void bioKillThreads(void) {}

void bioCreateCloseJob(int fd, int need_fsync, int need_reclaim_cache) {
    (void)need_reclaim_cache;
    if (need_fsync) fsync(fd);
    close(fd);
}
void bioCreateCloseAofJob(int fd, long long offset, int need_reclaim_cache) {
    (void)offset; (void)need_reclaim_cache;
    close(fd);
}
void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache) {
    (void)offset; (void)need_reclaim_cache;
    fsync(fd);
}
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    void *args[arg_count > 0 ? arg_count : 1];
    va_list ap; va_start(ap, arg_count);
    for (int i = 0; i < arg_count; i++) args[i] = va_arg(ap, void *);
    va_end(ap);
    free_fn(args);
}
/* New in 9.x. inBioThread: are we on a bio worker? Never — there are none. */
int inBioThread(void) { return 0; }
/* Dual-channel-replication RDB save on a bio thread. Replication is not used in
 * the sandbox; no-op (never reached on the ping/BullMQ paths). */
void bioCreateSaveRDBToDiskJob(void *conn, int is_dual_channel) {
    (void)conn; (void)is_dual_channel;
}

/* ==== I/O threads — forced to 1 (io-threads 1): always the main thread ======
 * io_threads.c is excluded. Every "try to offload" returns C_ERR (-1) so the
 * caller does the read/write/free itself, synchronously, right where it is. */
int inMainThread(void) { return 1; }
int getIOThreadID(void) { return 0; }
/* 9.x: initIOThreads takes the previous thread count (wasm checks the exact
 * function signature at the call site — a () vs (i32) mismatch traps). */
void initIOThreads(int prev_threads_num) { (void)prev_threads_num; }
void killIOThreads(void) {}
int trySendReadToIOThreads(void *c) { (void)c; return -1; }
int trySendWriteToIOThreads(void *c) { (void)c; return -1; }
int tryOffloadFreeObjToIOThreads(void *o) { (void)o; return -1; }
/* 9.x signature: (client *c, int argc, robj **argv). */
int tryOffloadFreeArgvToIOThreads(void *c, int argc, void **argv) { (void)c; (void)argc; (void)argv; return -1; }
void adjustIOThreadsByEventLoad(int numevents, int increase_only) { (void)numevents; (void)increase_only; }
void drainIOThreadsQueue(void) {}
void trySendPollJobToIOThreads(void) {}
/* New in 9.x io_threads API — still a single (main) thread, so all trivial. */
int  updateIOThreads(const char **err) { (void)err; return 0; /* C_OK: nothing to spin up */ }
long long getIOThreadActiveTimeMicroseconds(int id) { (void)id; return 0; }
int  clientHasPendingIO(void *c) { (void)c; return 0; }
int  processIOThreadsResponses(void) { return 0; }
void IOThreadsBeforeSleep(long long current_time) { (void)current_time; }
void IOThreadsAfterSleep(int numevents) { (void)numevents; }

/* ==== threads manager (crash-time cross-thread stack collection) ============
 * threads_mngr.c is excluded (pthreads). No other threads exist, so there is
 * nothing to run callbacks on. */
void ThreadsManager_init(void) {}
int ThreadsManager_runOnThreads(void *tids, size_t tids_len, void (*cb)(void)) {
    (void)tids; (void)tids_len; (void)cb; return 0;
}

/* waitForClientIO: blocks until a client's pending IO-thread work drains. With
 * io-threads forced to 1 everything is already done synchronously, so there is
 * never anything to wait for. */
void waitForClientIO(void *c) { (void)c; }

/* ==== libc calls wasi-libc omits, on paths that don't run here ==============
 * connect() lives in sock_shim.c (socket family). These are the rest: outgoing
 * process/pipe/file-lock/temp/timezone facilities Valkey references but that
 * the sandbox has no use for (no fork, no child procs, loopback-only). */
/* pipe() lives in sock_shim.c (it hands out fd-range ids like the socket fns). */
int   flock(int fd, int op) { (void)fd; (void)op; return 0; }
int   mkostemp(char *tmpl, int flags) { (void)tmpl; (void)flags; errno = ENOSYS; return -1; }
int   execve(const char *p, char *const av[], char *const ev[]) { (void)p; (void)av; (void)ev; errno = ENOSYS; return -1; }
unsigned umask(unsigned mask) { (void)mask; return 0; }
void  tzset(void) {}

/* dlopen family: module dynamic loading. Loadable .so modules aren't supported
 * in the sandbox (no shared objects) — dlopen of a *file* fails. But 9.x builds
 * Lua as a STATIC module and boots it via moduleLoadStatic(), which does
 * dlopen(NULL) (a handle to "self") + dlsym("ValkeyModule_OnLoad_lua"). There is
 * no dynamic symbol table in a wasm reactor, so we resolve that one known static
 * entry point directly: dlopen(NULL) returns a sentinel handle and dlsym returns
 * the linked address of the Lua engine's OnLoad. Everything else fails cleanly. */
extern int ValkeyModule_OnLoad_lua(void *ctx, void **argv, int argc);
static char self_handle; /* sentinel object; its address is the "self" handle */
void *dlopen(const char *file, int mode) {
    (void)mode;
    if (file == NULL) return &self_handle; /* handle to the running image */
    return 0;                              /* no file-backed modules */
}
void *dlsym(void *handle, const char *name) {
    (void)handle;
    if (name && strcmp(name, "ValkeyModule_OnLoad_lua") == 0)
        return (void *)&ValkeyModule_OnLoad_lua;
    return 0;
}
int   dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return "dynamic loading not supported"; }

/* libvalkey async client (9.x renamed hiredis' redisAsync* -> valkeyAsync*):
 * referenced only by sentinel.c, which never activates (the server never runs
 * in sentinel mode here). We don't build libvalkey, so stub what sentinel links
 * against. Fail/no-op. */
void *valkeyAsyncConnectBind(const char *ip, int port, const char *src) { (void)ip; (void)port; (void)src; return 0; }
int   valkeyAsyncCommand(void *ac, void *fn, void *pd, const char *fmt, ...) { (void)ac; (void)fn; (void)pd; (void)fmt; return -1; }
void  valkeyAsyncFree(void *ac) { (void)ac; }
int   valkeyAsyncSetConnectCallback(void *ac, void *cb) { (void)ac; (void)cb; return -1; }
int   valkeyAsyncSetDisconnectCallback(void *ac, void *cb) { (void)ac; (void)cb; return -1; }
void  valkeyAsyncHandleRead(void *ac) { (void)ac; }
void  valkeyAsyncHandleWrite(void *ac) { (void)ac; }

/* dup/dup2: wasi-libc omits them. Only referenced by daemonize() (server.c) and
 * valkey-check-rdb — neither runs in the reactor. Minimal success stubs. */
int dup(int oldfd) { return oldfd; }
int dup2(int oldfd, int newfd) { (void)oldfd; return newfd; }
