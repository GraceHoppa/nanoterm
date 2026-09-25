// ihex: Intel HEX -> flat memory image, the format arduino-cli writes.
#ifndef NANOTERM_IHEX_H
#define NANOTERM_IHEX_H

#include <stddef.h>
#include <stdio.h>

// Parse HEX records from f into img[0..max), filling untouched bytes with
// 0xFF (erased flash). Returns the image length (highest address written
// + 1), or -1 with a message in err (e.g. "line 3: checksum mismatch").
int ihex_read(FILE *f, unsigned char *img, int max, char *err, size_t errlen);

// Same, from a file path.
int ihex_load(const char *path, unsigned char *img, int max, char *err, size_t errlen);

#endif
