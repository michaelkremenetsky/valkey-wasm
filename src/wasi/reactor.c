/* Reactor entry points exported to the host (bridge/valkey-server.mjs).
 *
 * Valkey's main() is patched (server.c, VALKEY_WASM) to run full init and then
 * return instead of entering aeMain(). We call it once via rk_boot() to bring
 * the server up (globals stay resident in the instance), then the host drives
 * the event loop one non-blocking iteration at a time via rk_step() — on each
 * socket event and on a periodic timer (for serverCron, client + blocked-client
 * timeouts). host_read/host_write reach into this module's linear memory
 * directly through the exported `memory`, so no copy entry points are needed. */
#include "server.h"
#include "ae.h"
#include <stdio.h>
#include <string.h>

extern int main(int argc, char **argv);

__attribute__((export_name("rk_boot")))
int rk_boot(int port) {
    static char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    /* Dev-server config: no persistence (no fork), no threads, no
     * daemonize/pidfile, listen only on loopback via the host bridge. */
    char *argv[] = {
        "valkey-server",
        "--port", portbuf,
        "--bind", "127.0.0.1",
        "--save", "",
        "--appendonly", "no",
        "--protected-mode", "no",
        "--daemonize", "no",
        "--io-threads", "1",
        "--logfile", "",
        NULL
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;
    return main(argc, argv);
}

/* One non-blocking pass of the event loop. Returns the number of events
 * processed (0 means idle). */
__attribute__((export_name("rk_step")))
int rk_step(void) {
    return aeProcessEvents(server.el,
        AE_ALL_EVENTS | AE_DONT_WAIT | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP);
}

/* Milliseconds until the next timer event is due, so the host can size its next
 * timer instead of busy-polling. Returns -1 when only serverCron is pending. */
__attribute__((export_name("rk_next_timeout_ms")))
int rk_next_timeout_ms(void) {
    return 1000 / server.hz; /* serverCron cadence; good enough for a dev box */
}
