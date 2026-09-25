#include "ch340.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define VID 0x1a86
#define PID_CH340 0x7523
#define PID_CH341 0x5523
#define EP_IN 0x82
#define EP_OUT 0x02

#define REQ_READ_VERSION 0x5f
#define REQ_WRITE_REG 0x9a
#define REQ_SERIAL_INIT 0xa1
#define REQ_MODEM_CTRL 0xa4
#define REG_DIVISOR_PRESCALER 0x1312
#define REG_LCR 0x2518
#define LCR_8N1 0x00c3 // RX + TX enabled, 8 data bits, no parity, 1 stop

// Bit 7 of the divisor register: pass bytes on as they arrive instead of
// batching them into 32-byte packets (chip versions > 0x27, i.e. all the
// ones in circulation -- same condition the Linux driver checks).
#define DIVISOR_NO_BUFFER 0x80

uint16_t ch340_divisor(unsigned speed) {
    const unsigned clkrate = 48000000;
#define CLK_DIV(ps, fact) (1u << (12 - 3 * (ps) - (fact)))
    if (speed < 46 || speed > 3000000) return 0;

    // Highest base clock (fact = 1) that keeps the divisor under 512.
    unsigned fact = 1;
    int ps;
    for (ps = 3; ps > 0; ps--)
        if (speed > clkrate / (CLK_DIV(ps, 1) * 512)) break;

    unsigned clk_div = CLK_DIV(ps, fact);
    unsigned div = clkrate / (clk_div * speed);
    if (div < 9 || div > 255) { // halve the base clock
        div /= 2;
        clk_div *= 2;
        fact = 0;
    }
    if (div < 2) return 0;
    // Round to whichever divisor lands closer to the requested rate.
    if (16 * clkrate / (clk_div * div) - 16 * speed >= 16 * speed - 16 * clkrate / (clk_div * (div + 1)))
        div++;
    // Prefer the lower base clock when the divisor is even: same rate, and
    // the receiver is more tolerant of timing error.
    if (fact == 1 && div % 2 == 0) {
        div /= 2;
        fact = 0;
    }
#undef CLK_DIV
    return (uint16_t)((0x100 - div) << 8 | fact << 2 | (unsigned)ps);
}

static int ctrl_out(ch340 *c, uint8_t req, uint16_t val, uint16_t idx) {
    return libusb_control_transfer(c->dev, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
                                   req, val, idx, NULL, 0, 1000) < 0 ? -1 : 0;
}

static int port_write(port *p, const unsigned char *buf, int n) {
    ch340 *c = (ch340 *)p;
    while (n > 0) {
        int sent = 0;
        int r = libusb_bulk_transfer(c->dev, EP_OUT, (unsigned char *)buf, n, &sent, 2000);
        if (r != 0 && !(r == LIBUSB_ERROR_TIMEOUT && sent > 0)) return -1;
        buf += sent;
        n -= sent;
    }
    return 0;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int port_read(port *p, unsigned char *buf, int max, int timeout_ms) {
    ch340 *c = (ch340 *)p;
    long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1);
    // A zero-length packet completes a transfer with no data before the
    // timeout; keep waiting, so 0 returned really means "timed out".
    while (c->rxpos == c->rxlen) {
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        // Always ask for a whole buffer: a bulk read shorter than the
        // packet that arrives fails with LIBUSB_ERROR_OVERFLOW.
        int n = 0;
        int r = libusb_bulk_transfer(c->dev, EP_IN, c->rx, sizeof c->rx, &n, (unsigned)left);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) return -1;
        c->rxpos = 0;
        c->rxlen = n;
    }
    int n = c->rxlen - c->rxpos;
    if (n > max) n = max;
    memcpy(buf, c->rx + c->rxpos, n);
    c->rxpos += n;
    return n;
}

static int port_set_lines(port *p, int bits) {
    // The modem-control request takes the lines active-low.
    return ctrl_out((ch340 *)p, REQ_MODEM_CTRL, (uint16_t)~bits, 0);
}

static void port_sleep_ms(port *p, int ms) {
    (void)p;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int ch340_open(ch340 *c, int fd, char *err, size_t errlen) {
    int r;
    memset(c, 0, sizeof *c);
    c->base.write = port_write;
    c->base.read = port_read;
    c->base.set_lines = port_set_lines;
    c->base.sleep_ms = port_sleep_ms;

    if (fd >= 0) {
        // termux-usb hands us an open usbfs fd; there is no bus to scan.
        libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        if ((r = libusb_init(&c->ctx)) < 0) goto libusb_fail;
        if ((r = libusb_wrap_sys_device(c->ctx, (intptr_t)fd, &c->dev)) < 0) {
            snprintf(err, errlen, "can't use USB fd %d: %s", fd, libusb_error_name(r));
            ch340_close(c);
            return -1;
        }
    } else {
        if ((r = libusb_init(&c->ctx)) < 0) goto libusb_fail;
        c->dev = libusb_open_device_with_vid_pid(c->ctx, VID, PID_CH340);
        if (!c->dev) c->dev = libusb_open_device_with_vid_pid(c->ctx, VID, PID_CH341);
        if (!c->dev) {
            snprintf(err, errlen, "no CH340 board found (unplugged, or no permission -- see udev/99-ch340.rules)");
            ch340_close(c);
            return -1;
        }
        // Linux binds the ch341 driver (/dev/ttyUSB0); borrow the device
        // from it while we run, it gets it back on release.
        libusb_set_auto_detach_kernel_driver(c->dev, 1);
    }

    if ((r = libusb_claim_interface(c->dev, 0)) < 0) {
        snprintf(err, errlen, "can't claim the board: %s%s", libusb_error_name(r),
                 r == LIBUSB_ERROR_BUSY ? " (another program has it open?)" : "");
        ch340_close(c);
        return -1;
    }
    c->claimed = 1;
    return 0;

libusb_fail:
    snprintf(err, errlen, "libusb init failed: %s", libusb_error_name(r));
    ch340_close(c);
    return -1;
}

int ch340_set_baud(ch340 *c, unsigned baud) {
    uint16_t div = ch340_divisor(baud);
    if (!div) return -1;
    return ctrl_out(c, REQ_WRITE_REG, REG_DIVISOR_PRESCALER, div | DIVISOR_NO_BUFFER);
}

int ch340_configure(ch340 *c, unsigned baud, int lines) {
    unsigned char v[2];
    if (libusb_control_transfer(c->dev, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
                                REQ_READ_VERSION, 0, 0, v, 2, 1000) < 0)
        return -1;
    c->version = v[0];
    if (ctrl_out(c, REQ_SERIAL_INIT, 0, 0) < 0) return -1;
    if (ch340_set_baud(c, baud) < 0) return -1;
    if (ctrl_out(c, REQ_WRITE_REG, REG_LCR, LCR_8N1) < 0) return -1;
    return port_set_lines(&c->base, lines);
}

void ch340_close(ch340 *c) {
    if (c->claimed) libusb_release_interface(c->dev, 0);
    if (c->dev) libusb_close(c->dev);
    if (c->ctx) libusb_exit(c->ctx);
    memset(c, 0, sizeof *c);
}
