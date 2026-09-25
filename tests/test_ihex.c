// Intel HEX parser: real arduino-cli output must reproduce the program bytes
// exactly (tests/data/*.bin come from the ELF via avr-objcopy, independent
// of the .hex), and every kind of malformed input must be rejected with a
// useful message.
#include "ihex.h"

#include <stdlib.h>

#include "check.h"

#define MAX 30720

static unsigned char img[MAX];
static char err[256];

static int parse_str(const char *text) {
    FILE *f = fmemopen((void *)text, strlen(text), "r");
    int r = ihex_read(f, img, MAX, err, sizeof err);
    fclose(f);
    return r;
}

static void real_sketch(const char *name) {
    char hex[256], bin[256];
    snprintf(hex, sizeof hex, "tests/data/%s.ino.hex", name);
    snprintf(bin, sizeof bin, "tests/data/%s.ino.bin", name);

    FILE *f = fopen(bin, "rb");
    CHECK(f != NULL, "can't open %s", bin);
    if (!f) return;
    static unsigned char want[MAX];
    int want_len = (int)fread(want, 1, sizeof want, f);
    fclose(f);

    int n = ihex_load(hex, img, MAX, err, sizeof err);
    CHECK(n == want_len, "%s: length %d, expected %d (%s)", name, n, want_len, n < 0 ? err : "");
    if (n == want_len)
        CHECK(memcmp(img, want, (size_t)n) == 0, "%s: bytes differ from the .bin", name);
    CHECK(img[MAX - 1] == 0xff, "%s: unused flash should read as erased (0xFF)", name);
}

int main(void) {
    real_sketch("Blink");
    real_sketch("PhoneControl");

    // Minimal valid file; lowercase digits, CRLF endings and blank lines.
    CHECK(parse_str(":0400000001020304f2\r\n\n:00000001FF\r\n") == 4, "lowercase/CRLF: %s", err);
    CHECK(img[0] == 1 && img[3] == 4 && img[4] == 0xff, "lowercase/CRLF: wrong bytes");

    // Data at an offset; the gap stays 0xFF and the length is end+1.
    CHECK(parse_str(":02001000AABB89\n:00000001FF\n") == 0x12, "offset: %s", err);
    CHECK(img[0] == 0xff && img[0x10] == 0xAA && img[0x11] == 0xBB, "offset: wrong bytes");

    // Extended segment address (type 02): base = 0x0010 << 4 = 0x100.
    CHECK(parse_str(":020000020010EC\n:01000000AB54\n:00000001FF\n") == 0x101, "type 02: %s", err);
    CHECK(img[0x100] == 0xAB, "type 02: byte not at 0x100");

    // Extended linear address (type 04) into the 64K+ range: too big.
    CHECK(parse_str(":020000040001F9\n:01000000AB54\n:00000001FF\n") < 0, "type 04 >64K accepted");
    CHECK(contains(err, "too big"), "type 04: message was '%s'", err);

    // The with_bootloader.hex that sits next to the right file: must be
    // refused, it includes the bootloader region.
    CHECK(ihex_load("tests/data/Blink.ino.with_bootloader.hex", img, MAX, err, sizeof err) < 0,
          "with_bootloader.hex accepted");
    CHECK(contains(err, "too big"), "with_bootloader: message was '%s'", err);

    // Start-address records (03/05) are ignored, not errors.
    CHECK(parse_str(":0400000300000000F9\n:0100000055AA\n:00000001FF\n") == 1, "type 03: %s", err);

    CHECK(parse_str(":0100000055AB\n:00000001FF\n") < 0, "bad checksum accepted");
    CHECK(contains(err, "line 1") && contains(err, "checksum"), "checksum: message was '%s'", err);

    CHECK(parse_str(":0100000055AA\n:0100000G55AA\n:00000001FF\n") < 0, "non-hex digit accepted");
    CHECK(contains(err, "line 2"), "non-hex: message was '%s'", err);

    CHECK(parse_str(":0400000001AA\n:00000001FF\n") < 0, "short record accepted");
    CHECK(parse_str("hello\n") < 0, "garbage accepted");
    CHECK(parse_str(":0100000055AA\n") < 0, "missing EOF record accepted");
    CHECK(contains(err, "end-of-file"), "no EOF: message was '%s'", err);
    CHECK(parse_str(":00000001FF\n") < 0, "empty file accepted");
    CHECK(parse_str(":0100000200FD\n:00000001FF\n") < 0, "1-byte type 02 accepted");

    // Exactly at the limit is fine; one past is not.
    CHECK(parse_str(":0177FF00AADF\n:00000001FF\n") == MAX, "last byte of flash: %s", err);
    CHECK(parse_str(":01780000AADD\n:00000001FF\n") < 0, "byte past flash accepted");
    CHECK(contains(err, "too big"), "past flash: rejected for the wrong reason: '%s'", err);

    CHECK(ihex_load("tests/data/does-not-exist.hex", img, MAX, err, sizeof err) < 0, "missing file");

    TEST_MAIN_END("test_ihex");
}
