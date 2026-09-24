// nanoterm: tiny serial terminal / debugger for CH340-based Arduino clones,
// via libusb. 9600 baud, 8N1.
//
// Usage: nanoterm [-t] [-l FILE] [FD]
//   -t       prefix every received line with a timestamp
//   -l FILE  also append everything received and sent to FILE
//   FD       an already-open USB file descriptor (what termux-usb -e passes)
//
// Options can also come from the environment, for wrappers that cannot pass
// arguments through termux-usb: NANOTERM_TIMESTAMPS=1, NANOTERM_LOG=FILE.
//
// On Android (Termux, no root) use the `nt` wrapper, or by hand:
//   termux-usb -l                          # find the device path
//   termux-usb -r /dev/bus/usb/XXX/YYY     # grant permission
//   termux-usb -e ./nanoterm /dev/bus/usb/XXX/YYY
//
// On Linux it opens the first CH340 found, detaching the ch341 kernel driver
// while it runs (needs the udev rule in /etc/udev/rules.d/99-ch340.rules).
//
// Type a line and press Enter to send it (a '\n' is appended). Ctrl+C quits.

#include <errno.h>
#include <libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CH340_VID 0x1a86
#define CH340_PID 0x7523
#define EP_IN 0x82
#define EP_OUT 0x02
#define BAUD_9600_DIVISOR 0xb282 // divisor 0xb2, prescaler 2, "no buffer" bit
#define LCR_8N1 0x00c3           // RX + TX enabled, 8 data bits, no parity, 1 stop

static libusb_device_handle *dev;
static volatile sig_atomic_t running = 1;
static int timestamps;
static FILE *logfile;

static void on_sigint(int sig) { (void)sig; running = 0; }

static int ctrl_out(uint8_t req, uint16_t val, uint16_t idx) {
    return libusb_control_transfer(dev, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
                                   req, val, idx, NULL, 0, 1000);
}

static int ctrl_in(uint8_t req, uint16_t val, uint16_t idx, unsigned char *buf, uint16_t len) {
    return libusb_control_transfer(dev, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
                                   req, val, idx, buf, len, 1000);
}

static int ch340_init(void) {
    unsigned char buf[2];
    if (ctrl_in(0x5f, 0, 0, buf, 2) < 0) return -1;              // read chip version
    fprintf(stderr, "[nanoterm] CH340 version 0x%02x\n", buf[0]);
    if (ctrl_out(0xa1, 0, 0) < 0) return -1;                      // serial init
    if (ctrl_out(0x9a, 0x1312, BAUD_9600_DIVISOR) < 0) return -1; // baud rate
    if (ctrl_out(0x9a, 0x2518, LCR_8N1) < 0) return -1;           // line format
    if (ctrl_out(0xa4, (uint16_t)~(0x20 | 0x40), 0) < 0) return -1; // DTR+RTS on (resets the Nano)
    return 0;
}

// "12:34:56.789 " into buf
static void stamp(char *buf, size_t len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    size_t n = strftime(buf, len, "%H:%M:%S", &tm);
    snprintf(buf + n, len - n, ".%03ld ", (long)(tv.tv_usec / 1000));
}

// Output goes to stderr, not stdout: termux-usb -e captures the program's
// stdout with $(...) and only prints it after exit.
static void emit(const unsigned char *data, int n) {
    static int at_line_start = 1;
    char ts[32];
    for (int i = 0; i < n; i++) {
        if (at_line_start && timestamps) {
            stamp(ts, sizeof ts);
            fputs(ts, stderr);
            if (logfile) fputs(ts, logfile);
        }
        fputc(data[i], stderr);
        if (logfile) fputc(data[i], logfile);
        at_line_start = data[i] == '\n';
    }
    fflush(stderr);
    if (logfile) fflush(logfile);
}

static void *reader(void *arg) {
    (void)arg;
    unsigned char buf[256];
    while (running) {
        int n = 0;
        int r = libusb_bulk_transfer(dev, EP_IN, buf, sizeof buf, &n, 200);
        if (n > 0) emit(buf, n);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "\n[nanoterm] read error: %s\n", libusb_error_name(r));
            running = 0;
            kill(getpid(), SIGINT); // wake the main thread out of fgets()
        }
    }
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-t] [-l FILE] [FD]\n", prog);
    exit(2);
}

int main(int argc, char **argv) {
    libusb_context *ctx = NULL;
    const char *logpath = getenv("NANOTERM_LOG");
    const char *fdstr = getenv("TERMUX_USB_FD");
    int r, opt;

    timestamps = getenv("NANOTERM_TIMESTAMPS") != NULL;
    while ((opt = getopt(argc, argv, "tl:h")) != -1) {
        switch (opt) {
        case 't': timestamps = 1; break;
        case 'l': logpath = optarg; break;
        default: usage(argv[0]);
        }
    }
    if (optind < argc) fdstr = argv[argc - 1];

    if (logpath && *logpath && !(logfile = fopen(logpath, "a"))) {
        perror(logpath);
        return 1;
    }

    if (fdstr) {
        libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        if ((r = libusb_init(&ctx)) < 0) {
            fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r));
            return 1;
        }
        int fd = atoi(fdstr);
        if ((r = libusb_wrap_sys_device(ctx, (intptr_t)fd, &dev)) < 0) {
            fprintf(stderr, "libusb_wrap_sys_device(fd=%d): %s\n", fd, libusb_error_name(r));
            fprintf(stderr, "Run me via: termux-usb -e ./nanoterm /dev/bus/usb/XXX/YYY\n");
            return 1;
        }
    } else {
        if ((r = libusb_init(&ctx)) < 0) {
            fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r));
            return 1;
        }
        dev = libusb_open_device_with_vid_pid(ctx, CH340_VID, CH340_PID);
        if (!dev) {
            fprintf(stderr, "No CH340 device found (or no permission).\n");
            return 1;
        }
        libusb_set_auto_detach_kernel_driver(dev, 1);
    }

    if ((r = libusb_claim_interface(dev, 0)) < 0) {
        fprintf(stderr, "claim_interface: %s\n", libusb_error_name(r));
        return 1;
    }
    if (ch340_init() < 0) {
        fprintf(stderr, "CH340 init failed\n");
        return 1;
    }

    // No SA_RESTART, so Ctrl+C interrupts the blocking fgets() below instead
    // of waiting for the next Enter.
    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "[nanoterm] connected at 9600 baud%s%s%s. Type commands, Ctrl+C to quit.\n",
            timestamps ? ", timestamps on" : "",
            logfile ? ", logging to " : "", logfile ? logpath : "");

    pthread_t t;
    pthread_create(&t, NULL, reader, NULL);

    char line[256];
    while (running && fgets(line, sizeof line, stdin)) {
        size_t len = strcspn(line, "\r\n");
        line[len++] = '\n';
        if (logfile) {
            char ts[32] = "";
            if (timestamps) stamp(ts, sizeof ts);
            fprintf(logfile, "%s> %.*s", ts, (int)len, line);
            fflush(logfile);
        }
        int sent = 0;
        r = libusb_bulk_transfer(dev, EP_OUT, (unsigned char *)line, (int)len, &sent, 1000);
        if (r != 0) fprintf(stderr, "[nanoterm] write error: %s\n", libusb_error_name(r));
    }
    if (!running) fputc('\n', stderr);

    running = 0;
    pthread_join(t, NULL);
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(ctx);
    if (logfile) fclose(logfile);
    return 0;
}
