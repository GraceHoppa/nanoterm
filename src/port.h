// port: the byte-stream interface everything above the USB layer talks to.
// ch340.c implements it over libusb; tests/fake_optiboot.c implements it as
// a simulated bootloader, so the upload protocol is testable with no board.
#ifndef NANOTERM_PORT_H
#define NANOTERM_PORT_H

// Modem-control lines. DTR going active pulls the Nano's RESET low through
// its 100nF capacitor -- that edge is what restarts the board.
#define PORT_DTR 0x20
#define PORT_RTS 0x40

typedef struct port port;
struct port {
    // Write all n bytes. 0 on success, -1 on error.
    int (*write)(port *p, const unsigned char *buf, int n);
    // Read up to max bytes, waiting at most timeout_ms for the first one.
    // Returns bytes read, -1 on error, or 0 only once timeout_ms has passed
    // with nothing arriving -- callers rely on that and stop waiting.
    int (*read)(port *p, unsigned char *buf, int max, int timeout_ms);
    // Set DTR/RTS (PORT_DTR | PORT_RTS = both active). 0 or -1.
    int (*set_lines)(port *p, int bits);
    // Sleep; the fake port skips it so tests run instantly.
    void (*sleep_ms)(port *p, int ms);
};

#endif
