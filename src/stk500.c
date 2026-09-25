// Sequence and timing follow avrdude's "arduino" programmer
// (src/arduino.c + src/stk500.c), the thing `arduino-cli upload` runs.
#include "stk500.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CMD_GET_SYNC 0x30
#define CMD_ENTER_PROGMODE 0x50
#define CMD_LEAVE_PROGMODE 0x51
#define CMD_LOAD_ADDRESS 0x55
#define CMD_PROG_PAGE 0x64
#define CMD_READ_PAGE 0x74
#define CMD_READ_SIGN 0x75
#define CRC_EOP 0x20
#define RESP_INSYNC 0x14
#define RESP_NOSYNC 0x15
#define RESP_OK 0x10

#define SYNC_ATTEMPTS 10
#define REPLY_TIMEOUT_MS 500

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Exactly n bytes within timeout_ms total, or fewer on timeout / -1 on error.
static int read_exact(port *p, unsigned char *out, int n, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    int got = 0;
    while (got < n) {
        long left = deadline - now_ms();
        if (left <= 0) break;
        int r = p->read(p, out + got, n - got, (int)left);
        if (r < 0) return -1;
        if (r == 0) break; // the port waited out the whole timeout
        got += r;
    }
    return got;
}

// ------------------------------------------------------------------ trace --
// A port that forwards to another and logs everything, with milliseconds
// since the upload started: `nanoterm -v -u ...`.
typedef struct {
    port base;
    port *inner;
    FILE *f;
    long t0;
} trace_port;

static void trace_bytes(trace_port *t, const char *dir, const unsigned char *b, int n) {
    fprintf(t->f, "%6ld %s", now_ms() - t->t0, dir);
    for (int i = 0; i < n; i++) fprintf(t->f, " %02x", b[i]);
    fputc('\n', t->f);
}

static int trace_write(port *p, const unsigned char *buf, int n) {
    trace_port *t = (trace_port *)p;
    trace_bytes(t, "tx", buf, n > 16 ? 16 : n);
    if (n > 16) fprintf(t->f, "          ... %d bytes total\n", n);
    return t->inner->write(t->inner, buf, n);
}

static int trace_read(port *p, unsigned char *buf, int max, int timeout_ms) {
    trace_port *t = (trace_port *)p;
    int r = t->inner->read(t->inner, buf, max, timeout_ms);
    if (r > 0) trace_bytes(t, "rx", buf, r);
    else fprintf(t->f, "%6ld rx %s (waited %d ms)\n", now_ms() - t->t0, r < 0 ? "ERROR" : "nothing", timeout_ms);
    return r;
}

static int trace_set_lines(port *p, int bits) {
    trace_port *t = (trace_port *)p;
    fprintf(t->f, "%6ld DTR/RTS %s\n", now_ms() - t->t0, bits ? "active" : "released");
    return t->inner->set_lines(t->inner, bits);
}

static void trace_sleep(port *p, int ms) {
    trace_port *t = (trace_port *)p;
    fprintf(t->f, "%6ld sleep %d ms\n", now_ms() - t->t0, ms);
    t->inner->sleep_ms(t->inner, ms);
}

// Throw away whatever is in flight (the reset glitch, a running sketch's
// output, late replies) until the line has been quiet for 250ms -- avrdude's
// serial_drain_timeout. The wait matters, not just the discarding: optiboot
// flashes the LED for ~250ms after reset before it reads the UART, which
// holds only 3 bytes meanwhile, so commands sent sooner overrun it.
#define DRAIN_QUIET_MS 250
static void drain(port *p) {
    unsigned char junk[64];
    while (p->read(p, junk, sizeof junk, DRAIN_QUIET_MS) > 0) {}
}

void board_reset(port *p) {
    // Pulse, don't hold: per avrdude, keeping the line active charges the
    // reset capacitor and releasing it later spikes RESET above Vcc.
    p->set_lines(p, PORT_DTR | PORT_RTS);
    p->set_lines(p, 0);
}

// Send a command, then expect INSYNC, n reply bytes into reply, OK.
static int cmd(port *p, const unsigned char *c, int clen, unsigned char *reply, int n,
               char *err, size_t errlen) {
    unsigned char b;
    if (p->write(p, c, clen) < 0) {
        snprintf(err, errlen, "write to the board failed");
        return -1;
    }
    int r = read_exact(p, &b, 1, REPLY_TIMEOUT_MS);
    if (r != 1) {
        snprintf(err, errlen, "bootloader stopped answering (command 0x%02x)", c[0]);
        return -1;
    }
    if (b == RESP_NOSYNC) {
        snprintf(err, errlen, "bootloader lost sync (command 0x%02x)", c[0]);
        return -1;
    }
    if (b != RESP_INSYNC) {
        snprintf(err, errlen, "unexpected reply 0x%02x to command 0x%02x", b, c[0]);
        return -1;
    }
    if (n > 0 && read_exact(p, reply, n, REPLY_TIMEOUT_MS * 2) != n) {
        snprintf(err, errlen, "short reply to command 0x%02x", c[0]);
        return -1;
    }
    if (read_exact(p, &b, 1, REPLY_TIMEOUT_MS) != 1 || b != RESP_OK) {
        snprintf(err, errlen, "command 0x%02x not acknowledged", c[0]);
        return -1;
    }
    return 0;
}

static int getsync(port *p) {
    const unsigned char sync[] = { CMD_GET_SYNC, CRC_EOP };
    unsigned char b;

    // Flush line noise first: send and discard twice.
    for (int i = 0; i < 2; i++) {
        p->write(p, sync, sizeof sync);
        drain(p);
    }
    for (int attempt = 0; attempt < SYNC_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            // optiboot only listens for ~1s after reset; restart it rather
            // than keep talking to a sketch that has since started.
            board_reset(p);
            p->sleep_ms(p, 20);
            drain(p);
        }
        if (p->write(p, sync, sizeof sync) < 0) return -1;
        if (read_exact(p, &b, 1, REPLY_TIMEOUT_MS) == 1 && b == RESP_INSYNC &&
            read_exact(p, &b, 1, REPLY_TIMEOUT_MS) == 1 && b == RESP_OK)
            return 0;
    }
    return -1;
}

int stk500_upload(port *p, const unsigned char *img, int size, const stk500_opts *opts,
                  char *err, size_t errlen) {
    static const stk500_opts defaults;
    unsigned char reply[3], back[STK500_PAGE_SIZE];
    if (!opts) opts = &defaults;

    trace_port tp;
    if (opts->trace) {
        tp = (trace_port){ { trace_write, trace_read, trace_set_lines, trace_sleep }, p, opts->trace, now_ms() };
        p = &tp.base;
    }

    if (size <= 0 || size > STK500_FLASH_MAX) {
        snprintf(err, errlen, "image size %d out of range (1..%d)", size, STK500_FLASH_MAX);
        return -1;
    }

    // Lines inactive and settled, then pulse: the timing avrdude found
    // optiboot needs for back-to-back uploads to work.
    p->set_lines(p, 0);
    p->sleep_ms(p, 250);
    board_reset(p);
    p->sleep_ms(p, 100);
    drain(p);

    if (getsync(p) < 0) {
        snprintf(err, errlen, "no answer from the bootloader -- old-bootloader Nano (try the "
                              "other upload speed), not a Nano, or a program is holding the port");
        return -1;
    }

    const unsigned char sig[] = { CMD_READ_SIGN, CRC_EOP };
    if (cmd(p, sig, sizeof sig, reply, 3, err, errlen) < 0) return -1;
    // 1E 95 0F = ATmega328P, 1E 95 14 = ATmega328: same flash layout
    if (reply[0] != 0x1e || reply[1] != 0x95 || (reply[2] != 0x0f && reply[2] != 0x14)) {
        snprintf(err, errlen, "chip signature %02X %02X %02X is not an ATmega328(P)", reply[0],
                 reply[1], reply[2]);
        return -1;
    }

    const unsigned char enter[] = { CMD_ENTER_PROGMODE, CRC_EOP };
    if (cmd(p, enter, sizeof enter, NULL, 0, err, errlen) < 0) return -1;

    int pages = (size + STK500_PAGE_SIZE - 1) / STK500_PAGE_SIZE;
    for (int verifying = 0; verifying < 2; verifying++) {
        if (verifying && opts->skip_verify) break;
        for (int pg = 0; pg < pages; pg++) {
            unsigned addr = (unsigned)pg * STK500_PAGE_SIZE, word = addr / 2;
            const unsigned char load[] = { CMD_LOAD_ADDRESS, word & 0xff, word >> 8, CRC_EOP };
            if (cmd(p, load, sizeof load, NULL, 0, err, errlen) < 0) return -1;

            if (!verifying) {
                // Always a whole page; bytes past the image stay 0xFF (erased).
                unsigned char prog[4 + STK500_PAGE_SIZE + 1] = { CMD_PROG_PAGE, 0, STK500_PAGE_SIZE, 'F' };
                int n = size - (int)addr < STK500_PAGE_SIZE ? size - (int)addr : STK500_PAGE_SIZE;
                memset(prog + 4, 0xff, STK500_PAGE_SIZE);
                memcpy(prog + 4, img + addr, (size_t)n);
                prog[sizeof prog - 1] = CRC_EOP;
                if (cmd(p, prog, sizeof prog, NULL, 0, err, errlen) < 0) return -1;
            } else {
                const unsigned char rd[] = { CMD_READ_PAGE, 0, STK500_PAGE_SIZE, 'F', CRC_EOP };
                if (cmd(p, rd, sizeof rd, back, STK500_PAGE_SIZE, err, errlen) < 0) return -1;
                int n = size - (int)addr < STK500_PAGE_SIZE ? size - (int)addr : STK500_PAGE_SIZE;
                for (int i = 0; i < n; i++) {
                    if (back[i] != img[addr + (unsigned)i]) {
                        snprintf(err, errlen, "verify failed at 0x%04x: wrote %02X, read back %02X",
                                 addr + (unsigned)i, img[addr + (unsigned)i], back[i]);
                        return -1;
                    }
                }
            }
            if (opts->progress) opts->progress(opts->user, verifying, pg + 1, pages);
        }
    }

    // Leaving programming mode makes optiboot start the new sketch.
    const unsigned char leave[] = { CMD_LEAVE_PROGMODE, CRC_EOP };
    if (cmd(p, leave, sizeof leave, NULL, 0, err, errlen) < 0) return -1;
    return 0;
}
