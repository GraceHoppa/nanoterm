// The read-only guard for Betaflight's CLI: everything that only prints must
// pass, everything that changes the flight controller must not.
#include "betaflight.h"

#include "check.h"

static bf_kind kind(const char *s) { return bf_classify(s, strlen(s)); }

int main(void) {
    const char *reads[] = {
        "diff", "diff all", "dump", "dump hardware", "status", "version", "get",
        "get gyro_lpf1", "help", "tasks", "serial", "feature", "resource",
        "resource show", "timer show", "dma list", "profile", "rateprofile", "aux",
        "  diff all  ", "DIFF ALL", "Status",
    };
    for (size_t i = 0; i < sizeof reads / sizeof *reads; i++)
        CHECK(kind(reads[i]) == BF_READ, "'%s' should be read-only", reads[i]);

    const char *writes[] = {
        "save", "set gyro_lpf1_static_hz = 250", "defaults", "defaults nosave",
        "exit", "bl", "dfu", "flash_erase", "motor 0 1200", "serial 1 64 115200 57600 0 115200",
        "feature -AIRMODE", "feature GPS", "resource MOTOR 1 B00", "profile 2",
        "rateprofile 1", "aux 0 0 0 1700 2100 0 0", "name MyQuad", "mixer QUADX",
        "msc", "gpspassthrough", "play_sound", "batch start", "sav", "diffall",
        "unknowncommand", "timer A00 AF1",
    };
    for (size_t i = 0; i < sizeof writes / sizeof *writes; i++)
        CHECK(kind(writes[i]) == BF_WRITE, "'%s' should count as a write", writes[i]);

    CHECK(kind("") == BF_EMPTY, "empty line");
    CHECK(kind("   ") == BF_EMPTY, "blank line");
    // only the first len bytes count
    CHECK(bf_classify("save", 4) == BF_WRITE, "length-bounded save");
    CHECK(bf_classify("status save", 6) == BF_READ, "length cuts the line to 'status'");

    TEST_MAIN_END("test_betaflight");
}
