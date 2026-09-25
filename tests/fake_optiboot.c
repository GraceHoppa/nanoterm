#include "fake_optiboot.h"

#include <string.h>

#define CRC_EOP 0x20
#define INSYNC 0x14
#define OK 0x10

static void put(fake_optiboot *f, unsigned char b) {
    if (f->outhead == f->outtail) f->outhead = f->outtail = 0; // all read: rewind
    if (f->outtail < (int)sizeof f->out) f->out[f->outtail++] = b;
}

static void boot(fake_optiboot *f) {
    f->resets++;
    f->cmdlen = 0;
    f->outhead = f->outtail = 0;
    f->progmode = 0;
    if (f->resets <= f->ignore_resets) {
        // e.g. a flaky reset circuit: the pulse didn't take, the old
        // sketch is still running.
        f->in_bootloader = 0;
        f->app_started = 1;
        return;
    }
    f->in_bootloader = 1;
    f->app_started = 0;
    f->ready_at = f->clock_ms + f->startup_ms;
    f->uartlen = 0;
    for (int i = 0; i < f->noise_after_reset; i++) put(f, (unsigned char)(0xA5 ^ i));
}

// Bytes a command occupies, EOP included, once enough of it has arrived to
// tell (0 = need more). Mirrors optiboot's getch()/getNch() per command.
static int cmd_length(const fake_optiboot *f) {
    switch (f->cmd[0]) {
    case 0x41: return 3;  // GET_PARAMETER which EOP
    case 0x42: return 22; // SET_DEVICE: 20 bytes
    case 0x45: return 7;  // SET_DEVICE_EXT: 5 bytes
    case 0x55: return 4;  // LOAD_ADDRESS lo hi EOP
    case 0x56: return 6;  // UNIVERSAL: 4 bytes
    case 0x64:            // PROG_PAGE hi lo type data... EOP
        return f->cmdlen < 3 ? 0 : 5 + (f->cmd[1] << 8 | f->cmd[2]);
    case 0x74: return 5; // READ_PAGE hi lo type EOP
    default: return 2;   // everything else: cmd EOP
    }
}

static void handle(fake_optiboot *f) {
    const unsigned char *c = f->cmd;
    int len = f->cmdlen;
    f->commands++;
    if (f->die_after_commands && f->commands > f->die_after_commands) {
        f->in_bootloader = 0;
        return;
    }
    if (c[len - 1] != CRC_EOP) {
        // optiboot's verifySpace(): bad framing -> watchdog reset into the
        // application. No NOSYNC; the reset holds TX low, which the USB
        // chip reads as a 0x00 byte.
        f->in_bootloader = 0;
        f->app_started = 1;
        put(f, 0x00);
        return;
    }
    put(f, INSYNC);
    switch (c[0]) {
    case 0x41: put(f, c[1] == 0x81 ? 8 : c[1] == 0x82 ? 0 : 0x03); break;
    case 0x50: f->progmode = 1; break;
    case 0x51: f->progmode = 0; break;
    case 0x55: f->address = (unsigned)(c[1] | c[2] << 8) * 2; break; // words -> bytes
    case 0x56: put(f, 0x00); break;
    case 0x64: {
        unsigned n = (unsigned)(c[1] << 8 | c[2]);
        if (f->address + n <= sizeof f->flash) memcpy(f->flash + f->address, c + 4, n);
        break;
    }
    case 0x74: {
        unsigned n = (unsigned)(c[1] << 8 | c[2]);
        for (unsigned i = 0; i < n; i++) {
            unsigned a = f->address + i;
            unsigned char b = a < sizeof f->flash ? f->flash[a] : 0xff;
            if ((int)a == f->flip_readback_at) b ^= 0x01;
            put(f, b);
        }
        break;
    }
    case 0x75: put(f, f->sig[0]); put(f, f->sig[1]); put(f, f->sig[2]); break;
    default: break;
    }
    put(f, OK);
    if (c[0] == 0x51) { // 'Q': watchdog fires, the new sketch starts
        f->in_bootloader = 0;
        f->app_started = 1;
    }
}

// The bootloader reads one byte.
static void take(fake_optiboot *f, unsigned char b) {
    if (!f->in_bootloader) return; // the application ignores it
    if (f->cmdlen < (int)sizeof f->cmd) f->cmd[f->cmdlen++] = b;
    int need = cmd_length(f);
    if (need && f->cmdlen >= need) {
        handle(f);
        f->cmdlen = 0;
    }
}

// Once startup is over, the bytes the UART held meanwhile get read.
static void catch_up(fake_optiboot *f) {
    if (f->uartlen && f->clock_ms >= f->ready_at) {
        int n = f->uartlen;
        f->uartlen = 0;
        for (int i = 0; i < n; i++) take(f, f->uart[i]);
    }
}

static int fake_write(port *p, const unsigned char *buf, int n) {
    fake_optiboot *f = (fake_optiboot *)p;
    catch_up(f);
    for (int i = 0; i < n; i++) {
        if (f->dead || !f->in_bootloader) continue; // nobody listening
        if (f->clock_ms < f->ready_at) {
            // Still starting up: the UART holds 3 bytes, the rest overrun.
            if (f->uartlen < (int)sizeof f->uart) f->uart[f->uartlen++] = buf[i];
            continue;
        }
        take(f, buf[i]);
    }
    return 0;
}

static int fake_read(port *p, unsigned char *buf, int max, int timeout_ms) {
    fake_optiboot *f = (fake_optiboot *)p;
    catch_up(f);
    if (f->outhead == f->outtail && f->clock_ms < f->ready_at && f->uartlen) {
        // Waiting for a reply that comes once startup ends.
        long wait = f->ready_at - f->clock_ms;
        if (wait <= timeout_ms) {
            f->clock_ms = f->ready_at;
            catch_up(f);
        }
    }
    int n = 0;
    while (n < max && f->outhead < f->outtail) buf[n++] = f->out[f->outhead++];
    if (n == 0) f->clock_ms += timeout_ms; // waited out the whole timeout
    return n;
}

static int fake_set_lines(port *p, int bits) {
    fake_optiboot *f = (fake_optiboot *)p;
    // DTR going active pulls RESET low through the capacitor: a reset.
    if (!(f->lines & PORT_DTR) && (bits & PORT_DTR) && !f->dead) boot(f);
    f->lines = bits;
    return 0;
}

static void fake_sleep_ms(port *p, int ms) {
    fake_optiboot *f = (fake_optiboot *)p;
    f->clock_ms += ms;
    catch_up(f);
}

void fake_optiboot_init(fake_optiboot *f) {
    memset(f, 0, sizeof *f);
    f->base.write = fake_write;
    f->base.read = fake_read;
    f->base.set_lines = fake_set_lines;
    f->base.sleep_ms = fake_sleep_ms;
    f->sig[0] = 0x1e;
    f->sig[1] = 0x95;
    f->sig[2] = 0x0f;
    f->flip_readback_at = -1;
    memset(f->flash, 0x5a, sizeof f->flash); // "old sketch" contents
}
