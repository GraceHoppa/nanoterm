// term: the interactive serial terminal.
#ifndef NANOTERM_TERM_H
#define NANOTERM_TERM_H

#include <stdio.h>

#include "port.h"

typedef struct {
    FILE *out;      // where received data goes (stderr under termux-usb)
    FILE *log;      // optional copy of everything, sent lines marked "> "
    int timestamps; // "HH:MM:SS.mmm " at the start of each line
    int hex;        // show received bytes as hex, 16 per line
    const char *eol; // appended to each line typed ("\n", "\r\n", "\r", "")
    int duration;   // seconds before exiting on its own; 0 = until Ctrl+C
    // Betaflight CLI: enter it with '#' and check every typed line with
    // bf_classify(); lines that change the flight controller are refused,
    // or with allow_writes sent only after a "y" on the next line.
    int betaflight;
    int allow_writes;
    // Betaflight one-shot: send this command at the CLI prompt and return
    // when the prompt comes back (stdin is ignored).
    const char *oneshot;
} term_opts;

// Run until Ctrl+C / SIGTERM, the duration elapses, or the device goes
// away. Typed lines are sent; stdin reaching EOF just stops sending.
// Returns 0, or -1 if the device errored.
int term_run(port *p, const term_opts *o);

#endif
