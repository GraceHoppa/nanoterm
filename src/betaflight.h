// betaflight: which CLI lines only read a Betaflight flight controller and
// which change it. nanoterm sends the first kind freely; the second only
// with -W, and then only after you confirm each one.
#ifndef NANOTERM_BETAFLIGHT_H
#define NANOTERM_BETAFLIGHT_H

#include <stddef.h>

typedef enum {
    BF_EMPTY, // blank line
    BF_READ,  // prints something, changes nothing
    BF_WRITE, // changes settings or state (set, save, exit, a mode switch...)
} bf_kind;

// Classify one typed line (no line ending). Anything not known to be
// read-only counts as a write, so new or misspelled commands are refused too.
bf_kind bf_classify(const char *line, size_t len);

#endif
