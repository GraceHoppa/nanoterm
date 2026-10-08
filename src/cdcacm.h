// cdcacm: USB CDC ACM serial ("virtual COM port") driven directly through
// libusb. Betaflight/INAV flight controllers (STM32, AT32 VCP) and boards
// with native USB (Leonardo, Pro Micro, RP2040, ESP32-S3) all speak it.
#ifndef NANOTERM_CDCACM_H
#define NANOTERM_CDCACM_H

#include <libusb.h>
#include <stddef.h>

#include "port.h"

typedef struct {
    port base; // first, so a cdcacm* is usable as a port*
    libusb_context *ctx;
    libusb_device_handle *dev;
    int comm_if;                // control interface, -1 if the device has none
    int data_if;                // the interface with the two bulk endpoints
    unsigned char ep_in, ep_out;
    int claimed_comm, claimed_data;
    unsigned char rx[512];      // a whole bulk IN packet, full or high speed
    int rxpos, rxlen;
} cdcacm;

// Find the CDC interfaces in a configuration descriptor. 0 if found (comm_if
// may come back -1), -1 if the configuration has no CDC data interface.
int cdcacm_find(const struct libusb_config_descriptor *cfg, int *comm_if, int *data_if,
                unsigned char *ep_in, unsigned char *ep_out);

// Open the device. fd >= 0: an already-open usbfs descriptor (termux-usb);
// fd < 0: the first CDC ACM device on the bus (Linux). Returns 0, -1 on
// error, or -2 if the device isn't CDC ACM; err says why.
int cdcacm_open(cdcacm *a, int fd, char *err, size_t errlen);

// Line coding (baud, 8N1) and DTR/RTS. Flight controllers ignore the baud.
int cdcacm_configure(cdcacm *a, unsigned baud, int lines);

void cdcacm_close(cdcacm *a);

#endif
