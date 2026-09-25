// stk500: upload a flash image through the STK500v1 protocol spoken by
// optiboot, the bootloader on Arduino Nano/Uno boards -- the same exchange
// avrdude's "arduino" programmer performs.
#ifndef NANOTERM_STK500_H
#define NANOTERM_STK500_H

#include <stddef.h>
#include <stdio.h>

#include "port.h"

#define STK500_PAGE_SIZE 128  // ATmega328P flash page
#define STK500_FLASH_MAX 30720 // 32K minus the 2K bootloader section

typedef struct {
    // Called after each page is written (verifying = 0) or checked
    // (verifying = 1). May be NULL.
    void (*progress)(void *user, int verifying, int page, int pages);
    void *user;
    int skip_verify; // don't read the flash back afterwards
    FILE *trace;     // if set, log every byte sent/received and each step
} stk500_opts;

// Reset the board through DTR, sync with the bootloader, check it's an
// ATmega328(P), write img[0..size), read it back and compare, then start
// the sketch. The port must already be at the bootloader's baud rate.
// Returns 0, or -1 with the reason in err.
int stk500_upload(port *p, const unsigned char *img, int size, const stk500_opts *opts,
                  char *err, size_t errlen);

// Restart the board: a short DTR/RTS pulse, as avrdude does. The lines are
// left inactive afterwards (serial works either way).
void board_reset(port *p);

#endif
