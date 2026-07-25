/* wasi-sysroot has no <termios.h>: there is no controlling terminal in the
 * sandbox. Only memtest.c (interactive memory-test mode, never entered here)
 * and valkey-cli (not built) include it. Provide the type + no-op tc*attr so
 * the TU compiles; the terminal-raw-mode dance simply does nothing. On the -I
 * path (build.sh). */
#ifndef VALKEY_WASM_TERMIOS_H
#define VALKEY_WASM_TERMIOS_H
#include <sys/types.h>

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed, c_ospeed;
};

/* c_lflag bits / optional_actions referenced by callers */
#define ICANON  0000002
#define ECHO    0000010
#define TCSANOW    0
#define TCSADRAIN  1
#define TCSAFLUSH  2
#define VMIN  6
#define VTIME 5

static inline int tcgetattr(int fd, struct termios *t) { (void)fd; (void)t; return 0; }
static inline int tcsetattr(int fd, int act, const struct termios *t) {
    (void)fd; (void)act; (void)t; return 0;
}

/* winsize + ioctl(TIOCGWINSZ): wasi's <sys/ioctl.h> is empty. memtest.c queries
 * the terminal size for its progress bar; ioctl fails so it falls back to
 * 80x20, which is exactly what the sandbox wants (memtest never runs here). */
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
#define TIOCGWINSZ 0x5413
/* ioctl() itself is declared by wasi's <sys/ioctl.h>; memtest's TIOCGWINSZ
 * query just fails there and falls back to 80x20. */

#endif
