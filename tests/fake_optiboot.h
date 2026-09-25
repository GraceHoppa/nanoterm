// A simulated Arduino Nano running optiboot, behind the port interface.
// Modelled on optiboot.c: commands are answered INSYNC..OK; a command not
// terminated by CRC_EOP makes it go silent and start the application (it
// never sends NOSYNC); it only listens after a reset pulse.
#ifndef NANOTERM_FAKE_OPTIBOOT_H
#define NANOTERM_FAKE_OPTIBOOT_H

#include "port.h"

typedef struct {
    port base;

    // --- knobs set by the test ---
    unsigned char sig[3];   // chip signature (default ATmega328P)
    int ignore_resets;      // first N reset pulses don't start the bootloader
    int dead;               // never answers anything
    int noise_after_reset;  // bytes of junk sent right after each reset
    int die_after_commands; // go silent after N commands (0 = never)
    int flip_readback_at;   // corrupt this byte address on page reads (-1 = off)
    int startup_ms;         // after reset, busy (flashing the LED) this long
                            // before reading the UART -- which holds only 3
                            // bytes meanwhile; the rest are lost (overrun)

    // --- observable state ---
    unsigned char flash[32768];
    int resets;             // reset pulses seen
    int in_bootloader;      // listening for commands
    int app_started;        // left progmode / fell back to the application
    int commands;           // complete commands handled
    int progmode;

    // --- internals ---
    long clock_ms;  // simulated time: sleeps and read timeouts advance it
    long ready_at;  // when the bootloader starts reading commands
    unsigned char uart[3];
    int uartlen;
    int lines;
    unsigned address; // byte address
    unsigned char cmd[512];
    int cmdlen;
    unsigned char out[4096];
    int outhead, outtail;
} fake_optiboot;

void fake_optiboot_init(fake_optiboot *f);

#endif
