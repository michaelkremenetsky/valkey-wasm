/* libc socket family over host imports.
 *
 * Valkey's anet.c / socket.c call the standard POSIX socket calls. wasi-libc
 * has no real sockets, so we provide them here backed by host_* (node:net).
 * Host-socket fds are handed out from HOST_FD_BASE up, a range wasi-libc's own
 * fds never touch; the multiplexed calls (read/write/close/fcntl) are linked
 * with --wrap and dispatch to the host only for fds in that range, delegating
 * everything else to the real wasi-libc implementation.
 *
 * Only the subset anet.c actually uses is implemented; the rest are benign
 * stubs so the socket setup dance (nonblock, nodelay, reuseaddr, keepalive)
 * succeeds without doing anything the sandbox can't.
 */
#include "host.h"
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/uio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>

/* real wasi-libc entry points (renamed by --wrap) */
extern ssize_t __real_read(int, void *, size_t);
extern ssize_t __real_write(int, const void *, size_t);
extern ssize_t __real_writev(int, const struct iovec *, int);
extern int     __real_close(int);
extern int     __real_fcntl(int, int, ...);

static int is_host_fd(int fd) { return fd >= HOST_FD_BASE; }

/* ---- pipes ----------------------------------------------------------------
 * wasi-libc has no pipe(). The only pipe Valkey creates is server.module_pipe,
 * a self-wake channel for module background threads. No modules load here, so
 * nothing is ever written to it, but it must be created (its read end is armed
 * on the event loop) or startup aborts. Hand out fds from a small dedicated
 * range (below the host-socket range so they never collide with JS-allocated
 * fds) and treat them as an always-empty pipe: reads never have data (the read
 * end simply never fires in host_poll, which doesn't know these fds), writes
 * are discarded. */
#define PIPE_FD_BASE 3072
#define PIPE_FD_MAX  32
static int pipe_next = PIPE_FD_BASE;
static int is_pipe_fd(int fd) { return fd >= PIPE_FD_BASE && fd < PIPE_FD_BASE + PIPE_FD_MAX; }

int pipe(int fds[2]) {
    if (pipe_next + 1 >= PIPE_FD_BASE + PIPE_FD_MAX) { errno = EMFILE; return -1; }
    fds[0] = pipe_next++;
    fds[1] = pipe_next++;
    return 0;
}

/* ---- socket creation / setup (names wasi-libc leaves undefined) ---- */

/* JS owns fd allocation, so the whole lifecycle is: socket() gets a fresh host
 * fd from JS; bind() records the port for it (small side table); listen() tells
 * JS to open a net.Server on that fd+port; accept() returns the next JS-assigned
 * connection fd. anet uses the socket() fd throughout — no reconciliation. */
int socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    int fd = host_socket();
    if (fd < 0) { errno = EMFILE; return -1; }
    return fd;
}

/* tiny fd->port side table for the deferred listen (anet: socket→bind→listen) */
#define PORTMAP_MAX 16
static struct { int fd, port; } portmap[PORTMAP_MAX];
static void port_set(int fd, int port) {
    for (int i = 0; i < PORTMAP_MAX; i++)
        if (portmap[i].fd == 0 || portmap[i].fd == fd) { portmap[i].fd = fd; portmap[i].port = port; return; }
}
static int port_get(int fd) {
    for (int i = 0; i < PORTMAP_MAX; i++) if (portmap[i].fd == fd) return portmap[i].port;
    return 0;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)len;
    if (addr && addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        port_set(fd, __builtin_bswap16(in->sin_port));
    }
    return 0;
}

int listen(int fd, int backlog) {
    (void)backlog;
    if (host_listen(fd, port_get(fd)) < 0) { errno = EADDRINUSE; return -1; }
    return 0;
}

int accept(int sfd, struct sockaddr *addr, socklen_t *len) {
    (void)addr; if (len) *len = 0;
    int c = host_accept(sfd);
    if (c < 0) { errno = EAGAIN; return -1; }
    return c;
}
int accept4(int sfd, struct sockaddr *addr, socklen_t *len, int flags) {
    (void)flags; return accept(sfd, addr, len);
}

/* Outgoing connections (anet connect, cluster/replication link, sentinel) are
 * not used by the dev server — it only accepts loopback clients. Fail so any
 * such attempt reports a clean error instead of hanging. */
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)addr; (void)len; errno = ECONNREFUSED; return -1;
}

/* setup calls anet makes: all no-op successes on host sockets. */
int setsockopt(int fd, int lvl, int opt, const void *val, socklen_t len) {
    (void)fd; (void)lvl; (void)opt; (void)val; (void)len; return 0;
}
int getsockopt(int fd, int lvl, int opt, void *val, socklen_t *len) {
    (void)fd; (void)lvl; (void)opt;
    if (val && len && *len >= (socklen_t)sizeof(int)) { *(int *)val = 0; *len = sizeof(int); }
    return 0;
}
int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd; if (len) *len = 0; (void)addr; return 0;
}
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd; if (len) *len = 0; (void)addr; return 0;
}

/* ---- name resolution (no DNS in the sandbox; just shape the sockaddr) ----
 * anet.c's server/connect paths call getaddrinfo() before socket()/bind(). We
 * only ever bind/listen on loopback (the JS bridge is the real listener), so
 * getaddrinfo hands back a single 127.0.0.1:<service-port> IPv4 result, which
 * carries anet into the socket() shim where bind() records the port. IPv6 and
 * real hostname lookups are refused — the dev server binds 127.0.0.1 only. */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    (void)node;
    if (hints && hints->ai_family == AF_INET6) return EAI_FAMILY;

    struct addrinfo *ai = calloc(1, sizeof(*ai));
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    if (!ai || !sa) { free(ai); free(sa); return EAI_MEMORY; }

    sa->sin_family = AF_INET;
    sa->sin_port = __builtin_bswap16(service ? (uint16_t)atoi(service) : 0);
    sa->sin_addr.s_addr = __builtin_bswap32(INADDR_LOOPBACK);

    ai->ai_family = AF_INET;
    ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = 0;
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_next = 0;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode) { (void)errcode; return "name resolution disabled"; }

/* Valkey formats client/peer addresses with getnameinfo; every host socket is a
 * loopback connection through the bridge, so report 127.0.0.1 + the port. */
int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)salen; (void)flags;
    if (host && hostlen) {
        const char *h = "127.0.0.1";
        strncpy(host, h, hostlen - 1); host[hostlen - 1] = 0;
    }
    if (serv && servlen) {
        uint16_t port = 0;
        if (sa && sa->sa_family == AF_INET)
            port = __builtin_bswap16(((const struct sockaddr_in *)sa)->sin_port);
        snprintf(serv, servlen, "%u", port);
    }
    return 0;
}

/* ---- multiplexed calls: host fds -> host_*, else real wasi-libc ---- */

ssize_t __wrap_read(int fd, void *buf, size_t n) {
    if (is_host_fd(fd)) {
        int r = host_read(fd, buf, (int)n);
        if (r < 0) { errno = EAGAIN; return -1; }
        return r;
    }
    if (is_pipe_fd(fd)) { errno = EAGAIN; return -1; } /* pipe: never any data */
    return __real_read(fd, buf, n);
}
ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (is_host_fd(fd)) {
        int r = host_write(fd, buf, (int)n);
        if (r < 0) { errno = EPIPE; return -1; }
        return r;
    }
    if (is_pipe_fd(fd)) return (ssize_t)n; /* pipe: discard, report success */
    return __real_write(fd, buf, n);
}
/* Valkey gathers a client's multi-block reply into one writev() (networking.c
 * writevToClient) whenever the reply overflows the 16KB static buffer. The host
 * has no real fd, so fan the iovec out into host_write calls (node coalesces
 * them onto the socket). Without this, every reply larger than 16KB — INFO on a
 * loaded server, big EVAL results, BullMQ's script payloads — is silently
 * dropped, since the unwrapped writev would target a nonexistent wasi fd. */
ssize_t __wrap_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (is_host_fd(fd)) {
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (iov[i].iov_len == 0) continue;
            int r = host_write(fd, iov[i].iov_base, (int)iov[i].iov_len);
            if (r < 0) return total ? total : (errno = EPIPE, -1);
            total += r;
            if (r < (int)iov[i].iov_len) break; /* short write: stop here */
        }
        return total;
    }
    if (is_pipe_fd(fd)) {
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len; /* discard */
        return total;
    }
    return __real_writev(fd, iov, iovcnt);
}

int __wrap_close(int fd) {
    if (is_host_fd(fd)) { host_close(fd); return 0; }
    if (is_pipe_fd(fd)) return 0;
    return __real_close(fd);
}
int __wrap_fcntl(int fd, int cmd, ...) {
    if (is_host_fd(fd) || is_pipe_fd(fd)) return 0; /* always nonblocking here */
    /* forward the (at most one) int arg for real fds */
    __builtin_va_list ap; __builtin_va_start(ap, cmd);
    int arg = __builtin_va_arg(ap, int); __builtin_va_end(ap);
    return __real_fcntl(fd, cmd, arg);
}
