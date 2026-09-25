#include "ihex.h"

#include <errno.h>
#include <string.h>

// Hex digits at s -> value, or -1 if any isn't a hex digit.
static int hexval(const char *s, int digits) {
    int v = 0;
    for (int i = 0; i < digits; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                       : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

int ihex_read(FILE *f, unsigned char *img, int max, char *err, size_t errlen) {
    char line[600];
    int lineno = 0, end = 0, saw_eof = 0;
    unsigned long base = 0;

    memset(img, 0xff, (size_t)max);
    while (!saw_eof && fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\r' || *s == '\n' || !*s) continue;

        int count, addr, type;
        if (*s != ':' || (count = hexval(s + 1, 2)) < 0 || (addr = hexval(s + 3, 4)) < 0 ||
            (type = hexval(s + 7, 2)) < 0) {
            snprintf(err, errlen, "line %d: not an Intel HEX record", lineno);
            return -1;
        }
        // Every byte in the record, checksum included, sums to 0 mod 256.
        unsigned sum = (unsigned)(count + (addr >> 8) + (addr & 0xff) + type);
        unsigned char data[256];
        for (int i = 0; i <= count; i++) {
            int v = hexval(s + 9 + 2 * i, 2);
            if (v < 0) {
                snprintf(err, errlen, "line %d: record shorter than its byte count", lineno);
                return -1;
            }
            if (i < count) data[i] = (unsigned char)v;
            sum += (unsigned)v;
        }
        if (sum & 0xff) {
            snprintf(err, errlen, "line %d: checksum mismatch (corrupt file?)", lineno);
            return -1;
        }

        if ((type == 0x02 || type == 0x04) && count != 2) {
            snprintf(err, errlen, "line %d: address record must carry 2 bytes", lineno);
            return -1;
        }

        switch (type) {
        case 0x00: { // data
            unsigned long at = base + (unsigned long)addr;
            if (at + (unsigned long)count > (unsigned long)max) {
                snprintf(err, errlen, "sketch too big: needs %lu bytes, the board has %d",
                         at + (unsigned long)count, max);
                return -1;
            }
            memcpy(img + at, data, (size_t)count);
            if ((int)(at + (unsigned long)count) > end) end = (int)(at + (unsigned long)count);
            break;
        }
        case 0x01: saw_eof = 1; break;                                          // end of file
        case 0x02: base = (unsigned long)(data[0] << 8 | data[1]) << 4; break;   // segment base
        case 0x04: base = (unsigned long)(data[0] << 8 | data[1]) << 16; break;  // linear base
        default: break; // 03/05: start address, meaningless for AVR
        }
    }
    if (!saw_eof) {
        snprintf(err, errlen, "no end-of-file record (truncated file?)");
        return -1;
    }
    if (end == 0) {
        snprintf(err, errlen, "file contains no data");
        return -1;
    }
    return end;
}

int ihex_load(const char *path, unsigned char *img, int max, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "%s", strerror(errno));
        return -1;
    }
    int r = ihex_read(f, img, max, err, errlen);
    fclose(f);
    return r;
}
