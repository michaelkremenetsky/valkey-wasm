/* ae.c poll backend over host_poll. Included by ae.c when HAVE_WASI_AE is set
 * (see the small patch to ae.c's backend #include chain). Implements the same
 * 7 aeApi* functions as ae_select.c / ae_epoll.c.
 *
 * Unlike a native backend, aeApiPoll NEVER blocks: under Node the wasm runs on
 * the same thread as the event loop, so it cannot sleep waiting for I/O. JS
 * drives one aeProcessEvents(...AE_DONT_WAIT) iteration (reactor rk_step) each
 * time a socket becomes ready or a timer fires; aeApiPoll just reports what is
 * ready right now, which JS knows from its per-fd buffers. */
#include "host.h"

typedef struct aeApiState {
    /* interest mask per fd slot, indexed by (fd - HOST_FD_BASE) is impractical
     * (fds are large); instead we rebuild the interest list each poll from the
     * event loop's own events[] table, which already holds every fd's mask. */
    int dummy;
} aeApiState;

/* scratch buffers for the host_poll hand-off, sized to the loop's setsize */
static int32_t *ae_interest = 0;  /* (fd,mask) pairs */
static int32_t *ae_fired = 0;     /* (fd,mask) pairs */
static int ae_cap = 0;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;
    eventLoop->apidata = state;
    if (eventLoop->setsize > ae_cap) {
        ae_cap = eventLoop->setsize;
        ae_interest = zrealloc(ae_interest, sizeof(int32_t) * 2 * ae_cap);
        ae_fired = zrealloc(ae_fired, sizeof(int32_t) * 2 * ae_cap);
    }
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    if (setsize > ae_cap) {
        ae_cap = setsize;
        ae_interest = zrealloc(ae_interest, sizeof(int32_t) * 2 * ae_cap);
        ae_fired = zrealloc(ae_fired, sizeof(int32_t) * 2 * ae_cap);
    }
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    zfree(eventLoop->apidata);
}

/* Interest is tracked in eventLoop->events[] by ae.c itself; Add/Del are no-ops
 * here beyond what the core already records. We read events[] at poll time. */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    (void)eventLoop; (void)fd; (void)mask; return 0;
}
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    (void)eventLoop; (void)fd; (void)mask;
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    (void)tvp; /* never block */
    int n = 0;
    for (int fd = 0; fd <= eventLoop->maxfd; fd++) {
        int mask = eventLoop->events[fd].mask;
        if (mask == AE_NONE) continue;
        ae_interest[n * 2] = fd;
        ae_interest[n * 2 + 1] = mask;
        n++;
    }
    if (n == 0) return 0;
    int fired = host_poll(ae_interest, n, ae_fired, ae_cap, 0);
    for (int j = 0; j < fired; j++) {
        eventLoop->fired[j].fd = ae_fired[j * 2];
        eventLoop->fired[j].mask = ae_fired[j * 2 + 1];
    }
    return fired;
}

static char *aeApiName(void) { return "wasi-host"; }
