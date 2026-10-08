#include "cdcacm.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLASS_COMM 0x02
#define SUBCLASS_ACM 0x02
#define CLASS_DATA 0x0a

#define REQ_SET_LINE_CODING 0x20
#define REQ_SET_CONTROL_LINE_STATE 0x22
#define LINE_DTR 0x01
#define LINE_RTS 0x02

int cdcacm_find(const struct libusb_config_descriptor *cfg, int *comm_if, int *data_if,
                unsigned char *ep_in, unsigned char *ep_out) {
    *comm_if = *data_if = -1;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        if (itf->num_altsetting < 1) continue;
        const struct libusb_interface_descriptor *d = &itf->altsetting[0];
        if (d->bInterfaceClass == CLASS_COMM && d->bInterfaceSubClass == SUBCLASS_ACM && *comm_if < 0) {
            *comm_if = d->bInterfaceNumber;
            continue;
        }
        if (d->bInterfaceClass != CLASS_DATA || *data_if >= 0) continue;
        int in = 0, out = 0;
        for (int e = 0; e < d->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) in = ep->bEndpointAddress;
            else out = ep->bEndpointAddress;
        }
        if (in && out) {
            *data_if = d->bInterfaceNumber;
            *ep_in = (unsigned char)in;
            *ep_out = (unsigned char)out;
        }
    }
    return *data_if >= 0 ? 0 : -1;
}

static int ctrl_out(cdcacm *a, uint8_t req, uint16_t val, unsigned char *data, uint16_t len) {
    if (a->comm_if < 0) return 0; // no control interface: nothing to set
    return libusb_control_transfer(a->dev, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
                                   req, val, (uint16_t)a->comm_if, data, len, 1000) < 0 ? -1 : 0;
}

static int port_write(port *p, const unsigned char *buf, int n) {
    cdcacm *a = (cdcacm *)p;
    while (n > 0) {
        int sent = 0;
        int r = libusb_bulk_transfer(a->dev, a->ep_out, (unsigned char *)buf, n, &sent, 2000);
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

// Same contract as the CH340 read: 0 only once timeout_ms has passed.
static int port_read(port *p, unsigned char *buf, int max, int timeout_ms) {
    cdcacm *a = (cdcacm *)p;
    long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1);
    while (a->rxpos == a->rxlen) {
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        int n = 0;
        int r = libusb_bulk_transfer(a->dev, a->ep_in, a->rx, sizeof a->rx, &n, (unsigned)left);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) return -1;
        a->rxpos = 0;
        a->rxlen = n;
    }
    int n = a->rxlen - a->rxpos;
    if (n > max) n = max;
    memcpy(buf, a->rx + a->rxpos, n);
    a->rxpos += n;
    return n;
}

static int port_set_lines(port *p, int bits) {
    uint16_t v = (bits & PORT_DTR ? LINE_DTR : 0) | (bits & PORT_RTS ? LINE_RTS : 0);
    return ctrl_out((cdcacm *)p, REQ_SET_CONTROL_LINE_STATE, v, NULL, 0);
}

static void port_sleep_ms(port *p, int ms) {
    (void)p;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Read the configuration and claim the interfaces of an opened device.
static int attach(cdcacm *a, char *err, size_t errlen) {
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(libusb_get_device(a->dev), &cfg);
    if (r < 0) {
        snprintf(err, errlen, "can't read the USB descriptors: %s", libusb_error_name(r));
        return -1;
    }
    r = cdcacm_find(cfg, &a->comm_if, &a->data_if, &a->ep_in, &a->ep_out);
    libusb_free_config_descriptor(cfg);
    if (r < 0) {
        snprintf(err, errlen, "not a USB serial (CDC ACM) device");
        return -2;
    }
    // Linux binds cdc_acm (/dev/ttyACM0); borrow the device while we run.
    libusb_set_auto_detach_kernel_driver(a->dev, 1);
    if (a->comm_if >= 0) {
        // Some devices refuse; the data interface alone still moves bytes.
        if (libusb_claim_interface(a->dev, a->comm_if) == 0) a->claimed_comm = 1;
        else a->comm_if = -1;
    }
    if ((r = libusb_claim_interface(a->dev, a->data_if)) < 0) {
        snprintf(err, errlen, "can't claim the device: %s%s", libusb_error_name(r),
                 r == LIBUSB_ERROR_BUSY ? " (another program has it open?)" : "");
        return -1;
    }
    a->claimed_data = 1;
    return 0;
}

int cdcacm_open(cdcacm *a, int fd, char *err, size_t errlen) {
    int r;
    memset(a, 0, sizeof *a);
    a->base.write = port_write;
    a->base.read = port_read;
    a->base.set_lines = port_set_lines;
    a->base.sleep_ms = port_sleep_ms;
    a->comm_if = a->data_if = -1;

    if (fd >= 0) {
        libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        if ((r = libusb_init(&a->ctx)) < 0) goto libusb_fail;
        if ((r = libusb_wrap_sys_device(a->ctx, (intptr_t)fd, &a->dev)) < 0) {
            snprintf(err, errlen, "can't use USB fd %d: %s", fd, libusb_error_name(r));
            cdcacm_close(a);
            return -1;
        }
    } else {
        if ((r = libusb_init(&a->ctx)) < 0) goto libusb_fail;
        libusb_device **list;
        ssize_t n = libusb_get_device_list(a->ctx, &list);
        for (ssize_t i = 0; i < n && !a->dev; i++) {
            struct libusb_config_descriptor *cfg;
            if (libusb_get_active_config_descriptor(list[i], &cfg) < 0) continue;
            int c, d;
            unsigned char ei, eo;
            int found = cdcacm_find(cfg, &c, &d, &ei, &eo) == 0;
            libusb_free_config_descriptor(cfg);
            if (found && libusb_open(list[i], &a->dev) < 0) a->dev = NULL;
        }
        if (n >= 0) libusb_free_device_list(list, 1);
        if (!a->dev) {
            snprintf(err, errlen, "no USB serial (CDC ACM) device found");
            cdcacm_close(a);
            return -2;
        }
    }

    if ((r = attach(a, err, errlen)) < 0) {
        cdcacm_close(a);
        return r;
    }
    return 0;

libusb_fail:
    snprintf(err, errlen, "libusb init failed: %s", libusb_error_name(r));
    cdcacm_close(a);
    return -1;
}

int cdcacm_configure(cdcacm *a, unsigned baud, int lines) {
    // dwDTERate (little-endian), 1 stop bit, no parity, 8 data bits
    unsigned char coding[7] = { baud & 0xff, (baud >> 8) & 0xff, (baud >> 16) & 0xff, (baud >> 24) & 0xff, 0, 0, 8 };
    if (ctrl_out(a, REQ_SET_LINE_CODING, 0, coding, sizeof coding) < 0) return -1;
    return port_set_lines(&a->base, lines);
}

void cdcacm_close(cdcacm *a) {
    if (a->claimed_data) libusb_release_interface(a->dev, a->data_if);
    if (a->claimed_comm) libusb_release_interface(a->dev, a->comm_if);
    if (a->dev) libusb_close(a->dev);
    if (a->ctx) libusb_exit(a->ctx);
    memset(a, 0, sizeof *a);
    a->comm_if = a->data_if = -1;
}
