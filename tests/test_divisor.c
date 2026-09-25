// CH340 baud-rate register: the two values verified on real hardware must
// come out exactly, and every common rate must land within the ~2% a UART
// tolerates.
#include "ch340.h"

#include <math.h>

#include "check.h"

// Rate the chip will actually produce for a register value -- decoded
// independently of how ch340_divisor() computed it.
static double actual_rate(uint16_t v) {
    unsigned ps = v & 3, fact = (v >> 2) & 1, div = 0x100 - (v >> 8);
    return 48000000.0 / ((double)(1u << (12 - 3 * ps - fact)) * div);
}

int main(void) {
    // Values the Linux ch341 driver computes and that ran on a real Nano.
    CHECK(ch340_divisor(9600) == 0xb202, "9600 -> 0x%04x, want 0xb202", ch340_divisor(9600));
    CHECK(ch340_divisor(115200) == 0xcc03, "115200 -> 0x%04x, want 0xcc03", ch340_divisor(115200));
    CHECK(ch340_divisor(57600) == 0x9803, "57600 -> 0x%04x, want 0x9803", ch340_divisor(57600));

    const unsigned rates[] = { 300,   1200,  2400,   4800,   9600,   14400,  19200,  28800,
                               31250, 38400, 57600,  74880,  115200, 230400, 250000, 460800,
                               500000, 921600, 1000000, 2000000 };
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        uint16_t v = ch340_divisor(rates[i]);
        CHECK(v != 0, "%u baud rejected", rates[i]);
        if (!v) continue;
        double err = fabs(actual_rate(v) - rates[i]) / rates[i] * 100;
        CHECK(err < 2.0, "%u baud comes out as %.0f (%.2f%% off)", rates[i], actual_rate(v), err);
    }

    CHECK(ch340_divisor(0) == 0, "0 baud accepted");
    CHECK(ch340_divisor(10) == 0, "10 baud accepted (chip minimum is ~46)");
    CHECK(ch340_divisor(5000000) == 0, "5M baud accepted (chip maximum is 3M)");

    TEST_MAIN_END("test_divisor");
}
