/* wasi-sysroot has no <netdb.h>. anet.c uses getaddrinfo/getnameinfo to turn a
 * bind address + port into a sockaddr before socket()/bind()/listen(). Real DNS
 * resolution is meaningless in the sandbox (the JS bridge owns the actual
 * listener), but the *shape* matters: getaddrinfo must succeed and hand back a
 * 127.0.0.1:port sockaddr so anet proceeds into the socket() shim, where bind()
 * records the port and listen() calls host_listen. The implementations live in
 * sock_shim.c; this header just declares the surface. On the -I path (build.sh). */
#ifndef VALKEY_WASM_NETDB_H
#define VALKEY_WASM_NETDB_H
#include <sys/socket.h>
#include <netinet/in.h>

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

/* getaddrinfo() ai_flags */
#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x400
#define AI_V4MAPPED    0x08
#define AI_ALL         0x10
#define AI_ADDRCONFIG  0x20

/* getnameinfo() flags */
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

/* getaddrinfo() error codes */
#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_SOCKTYPE   -7
#define EAI_SERVICE    -8
#define EAI_MEMORY     -10
#define EAI_SYSTEM     -11

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int  getnameinfo(const struct sockaddr *sa, socklen_t salen,
                 char *host, socklen_t hostlen,
                 char *serv, socklen_t servlen, int flags);

#endif
