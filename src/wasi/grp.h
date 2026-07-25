/* wasi-sysroot has no <grp.h>: there is no user/group database in the sandbox.
 * anet.c includes it only for anetUnixServer's optional group-ownership chown,
 * a path that never runs here (no unix socket is configured). getgrnam() just
 * reports "no such group". On the -I path (build.sh). */
#ifndef VALKEY_WASM_GRP_H
#define VALKEY_WASM_GRP_H
#include <sys/types.h>

struct group {
    char  *gr_name;
    char  *gr_passwd;
    gid_t  gr_gid;
    char **gr_mem;
};

static inline struct group *getgrnam(const char *name) { (void)name; return 0; }

#endif
