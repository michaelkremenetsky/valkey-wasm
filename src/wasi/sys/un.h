/* wasi-sysroot's <sys/un.h> declares a struct sockaddr_un with only sun_family
 * (there are no unix-domain sockets in the sandbox). anet.c / unix.c reference
 * sun_path unconditionally, so this shadowing header (on the -I path, ahead of
 * the sysroot) supplies the full BSD shape. Unix sockets are never created at
 * runtime here — this only needs to compile. */
#ifndef VALKEY_WASM_SYS_UN_H
#define VALKEY_WASM_SYS_UN_H
#include <sys/socket.h>   /* sa_family_t */

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};

#endif
