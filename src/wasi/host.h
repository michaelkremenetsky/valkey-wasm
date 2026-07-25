/* Host-import contract. Every function here is implemented in JS
 * (bridge/valkey-server.mjs) and imported into the wasm instance under the
 * module name "host". This is the ENTIRE surface between Valkey and the
 * outside world: sockets + the event-loop poll. Keep it tiny and stable. */
#ifndef VALKEY_WASM_HOST_H
#define VALKEY_WASM_HOST_H
#include <stddef.h>
#include <stdint.h>

#define HOST_IMPORT(name) __attribute__((import_module("host"), import_name(name)))

/* Host-socket fds live at or above this base. It must be SMALL: ae.c indexes
 * eventLoop->events[fd] directly, so every fd has to fit under the loop setsize
 * (maxclients + reserved, ~10k by default). 4096 clears wasi-libc's own fds
 * (stdio + the handful of files a server opens stay < ~100) while leaving room
 * for thousands of connections below setsize. JS allocates host fds from here
 * up; read/write/close dispatch on `fd >= HOST_FD_BASE`. */
#define HOST_FD_BASE 4096

/* ---- sockets (JS owns the fd namespace + a net.Server + per-fd buffers) ----
 * JS is the fd authority: it allocates every host fd (listeners and accepted
 * connections alike) from a single counter at HOST_FD_BASE, so C never has to
 * reconcile ids. */
/* Allocate an unbound host socket fd. */
HOST_IMPORT("socket")  int   host_socket(void);
/* Turn fd into a listener on 127.0.0.1:port (net.Server). 0 ok, -1 on error. */
HOST_IMPORT("listen")  int   host_listen(int fd, int port);
/* Pull the next accepted connection off listener lfd; returns its (JS-assigned)
 * fd, or -1 if none pending. */
HOST_IMPORT("accept")  int   host_accept(int lfd);
/* Copy up to len buffered bytes for fd into buf. Returns n>0, 0 on EOF,
 * -1 (EAGAIN) when nothing is buffered yet. */
HOST_IMPORT("read")    int   host_read(int fd, void *buf, int len);
/* Queue len bytes from buf to fd's socket. Returns n written (node buffers,
 * so normally == len) or -1 on a dead socket. */
HOST_IMPORT("write")   int   host_write(int fd, const void *buf, int len);
/* Close/destroy fd's socket. */
HOST_IMPORT("close")   void  host_close(int fd);

/* ---- event-loop poll (never blocks; see ae_wasi.c) ----
 * C writes its interest set as (int32 fd, int32 mask) pairs at interest[],
 * count entries. JS fills fired[] with the subset ready RIGHT NOW (readable
 * = has buffered data or EOF; writable = socket not backpressured) and returns
 * the fired count. timeout_ms is advisory and ignored (Node owns real waiting).
 * mask bits match ae.h: AE_READABLE=1, AE_WRITABLE=2. */
HOST_IMPORT("poll")    int   host_poll(const int32_t *interest, int count,
                                       int32_t *fired, int max, int timeout_ms);

#endif
