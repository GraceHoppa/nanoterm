// ch340: CH340/CH341 USB-serial chip driven directly through libusb -- no
// kernel driver, which is what makes this work unrooted in Termux.
#ifndef NANOTERM_CH340_H
#define NANOTERM_CH340_H

#include <libusb.h>
#include <stddef.h>
#include <stdint.h>

#include "port.h"

typedef struct {
    port base; // first, so a ch340* is usable as a port*
    libusb_context *ctx;
    libusb_device_handle *dev;
    int claimed;
    int version;               // chip version byte, e.g. 0x31
    unsigned char rx[64];      // bulk IN packets arrive whole; keep the rest
    int rxpos, rxlen;
} ch340;

// Open the chip. fd >= 0: an already-open usbfs descriptor (termux-usb);
// fd < 0: find the first CH340/CH341 on the bus (Linux). On failure returns
// -1 and puts a human-readable reason in err.
int ch340_open(ch340 *c, int fd, char *err, size_t errlen);

// Initialise the UART: baud rate, 8N1, and the DTR/RTS state (pass
// PORT_DTR | PORT_RTS to reset the board, 0 to leave it running).
int ch340_configure(ch340 *c, unsigned baud, int lines);

int ch340_set_baud(ch340 *c, unsigned baud);

void ch340_close(ch340 *c);

// Divisor/prescaler register value for a baud rate, without the "no buffer"
// bit. A port of ch341_get_divisor() from Linux drivers/usb/serial/ch341.c.
// Returns 0 for rates the chip can't do (outside ~46..3000000).
uint16_t ch340_divisor(unsigned baud);

#endif
