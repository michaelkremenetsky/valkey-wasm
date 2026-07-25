/* wasi-sysroot has no <sys/wait.h>: there are no child processes in the
 * sandbox. Valkey only reaps children spawned by fork() for BGSAVE / AOF
 * rewrite / module forks, all of which are disabled here (fork() is macro'd to
 * -1, persistence is off). These declarations exist purely so the ~6 TUs that
 * include the header compile; the status-inspection macros are never reached
 * because no wait ever returns a live child. On the -I path (build.sh). */
#ifndef VALKEY_WASM_SYS_WAIT_H
#define VALKEY_WASM_SYS_WAIT_H
#include <sys/types.h>

/* waitpid()/wait3() are macro'd to -1 in wasm_compat.h; the status macros below
 * operate on that (never-populated) status word. */
#define WNOHANG   1
#define WUNTRACED 2

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WCOREDUMP(s)    ((s) & 0x80)
#define WIFCONTINUED(s) ((s) == 0xffff)

#endif
