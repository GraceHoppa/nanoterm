// The real STK500 upload code against a simulated optiboot: the happy path,
// and each way a real upload goes wrong.
#include "stk500.h"

#include "check.h"
#include "fake_optiboot.h"

static fake_optiboot f;
static unsigned char img[STK500_FLASH_MAX];
static char err[256];
static int progress_calls, last_verifying, last_page, last_pages;

static void on_progress(void *user, int verifying, int page, int pages) {
    (void)user;
    progress_calls++;
    last_verifying = verifying;
    last_page = page;
    last_pages = pages;
}

static void fill(int size) {
    for (int i = 0; i < size; i++) img[i] = (unsigned char)(i * 7 + 3);
}

static int upload(int size) {
    stk500_opts o = { .progress = on_progress };
    err[0] = '\0';
    progress_calls = 0;
    return stk500_upload(&f.base, img, size, &o, err, sizeof err);
}

int main(void) {
    // Happy path, partial last page: 1000 bytes = 7 full pages + 104 bytes.
    fake_optiboot_init(&f);
    fill(1000);
    CHECK(upload(1000) == 0, "upload failed: %s", err);
    CHECK(memcmp(f.flash, img, 1000) == 0, "flash doesn't match the image");
    CHECK(f.flash[1000] == 0xff && f.flash[1023] == 0xff,
          "rest of the last page should be erased (0xFF), got %02x", f.flash[1000]);
    CHECK(f.flash[1024] == 0x5a, "page after the image should be untouched");
    CHECK(f.app_started && !f.in_bootloader, "sketch not started after upload");
    CHECK(f.resets == 1, "expected exactly one reset, got %d", f.resets);
    CHECK(f.lines == 0, "DTR/RTS left active (0x%x): reset line should be released", f.lines);
    CHECK(progress_calls == 16 && last_verifying == 1 && last_page == 8 && last_pages == 8,
          "progress: %d calls, last %d/%d verifying=%d", progress_calls, last_page, last_pages,
          last_verifying);

    // Exact page multiple, and the largest sketch that fits.
    fake_optiboot_init(&f);
    fill(STK500_FLASH_MAX);
    CHECK(upload(STK500_FLASH_MAX) == 0, "max-size upload failed: %s", err);
    CHECK(memcmp(f.flash, img, STK500_FLASH_MAX) == 0, "max-size: flash mismatch");
    CHECK(f.flash[STK500_FLASH_MAX] == 0x5a, "wrote into the bootloader section");

    // Line noise / leftover sketch output right after reset is drained.
    fake_optiboot_init(&f);
    f.noise_after_reset = 40;
    fill(300);
    CHECK(upload(300) == 0, "noise after reset broke the upload: %s", err);

    // A real Nano (captured with `nanoterm -v`): the reset reads as 32 zero
    // bytes, then optiboot flashes the LED for ~250ms before it reads the
    // UART, which only holds 3 bytes meanwhile. Syncs sent too quickly
    // overrun it, leave half a command behind, and the next real command
    // makes optiboot bail out. Regression test for exactly that.
    for (int startup = 0; startup <= 400; startup += 50) {
        fake_optiboot_init(&f);
        f.startup_ms = startup;
        f.noise_after_reset = 32;
        fill(922);
        CHECK(upload(922) == 0, "startup %d ms: %s", startup, err);
        CHECK(memcmp(f.flash, img, 922) == 0, "startup %d ms: flash mismatch", startup);
    }

    // Reset pulses that don't take: sync retries re-pulse until one does.
    fake_optiboot_init(&f);
    f.ignore_resets = 3;
    fill(200);
    CHECK(upload(200) == 0, "retry path failed: %s", err);
    CHECK(f.resets == 4, "expected 4 resets (3 ignored), got %d", f.resets);
    CHECK(memcmp(f.flash, img, 200) == 0, "retry path: flash mismatch");

    // Board that never answers (not a Nano, wrong speed, bootloader gone).
    fake_optiboot_init(&f);
    f.dead = 1;
    fill(200);
    CHECK(upload(200) < 0, "dead board reported success");
    CHECK(contains(err, "no answer"), "dead board: message was '%s'", err);

    // Gives up after the retry budget rather than forever.
    fake_optiboot_init(&f);
    f.ignore_resets = 1000;
    CHECK(upload(200) < 0, "never-starting bootloader reported success");
    CHECK(f.resets == 10, "expected 10 sync attempts (resets), got %d", f.resets);

    // Wrong chip (ATmega168: 1E 94 06).
    fake_optiboot_init(&f);
    f.sig[1] = 0x94;
    f.sig[2] = 0x06;
    CHECK(upload(200) < 0, "wrong chip accepted");
    CHECK(contains(err, "1E 94 06"), "wrong chip: message was '%s'", err);
    CHECK(f.flash[0] == 0x5a, "wrote flash despite the wrong signature");

    // ATmega328 (non-P) has the same layout: accepted.
    fake_optiboot_init(&f);
    f.sig[2] = 0x14;
    fill(200);
    CHECK(upload(200) == 0, "ATmega328 (non-P) rejected: %s", err);

    // A corrupted read-back must fail the verify and say where.
    fake_optiboot_init(&f);
    f.flip_readback_at = 300;
    fill(600);
    CHECK(upload(600) < 0, "corrupt read-back passed verification");
    CHECK(contains(err, "0x012c"), "verify: message should name address 0x012c, was '%s'", err);

    // Corruption past the image in the padded last page doesn't matter.
    fake_optiboot_init(&f);
    f.flip_readback_at = 610;
    fill(600);
    CHECK(upload(600) == 0, "padding byte failed verify: %s", err);

    // Bootloader dies mid-upload.
    fake_optiboot_init(&f);
    f.die_after_commands = 10;
    fill(3000);
    CHECK(upload(3000) < 0, "mid-upload death reported success");
    CHECK(contains(err, "stopped answering"), "mid-upload: message was '%s'", err);

    // skip_verify: no page reads.
    fake_optiboot_init(&f);
    f.flip_readback_at = 5;
    fill(200);
    stk500_opts nv = { .skip_verify = 1 };
    CHECK(stk500_upload(&f.base, img, 200, &nv, err, sizeof err) == 0, "skip_verify: %s", err);

    // Nonsense sizes are refused before touching the board.
    fake_optiboot_init(&f);
    CHECK(upload(0) < 0 && f.resets == 0, "size 0 accepted or board reset");
    CHECK(upload(STK500_FLASH_MAX + 1) < 0 && f.resets == 0, "oversize accepted or board reset");

    // Reset helper on its own: one pulse, lines released.
    fake_optiboot_init(&f);
    board_reset(&f.base);
    CHECK(f.resets == 1 && f.lines == 0, "board_reset: %d resets, lines 0x%x", f.resets, f.lines);

    TEST_MAIN_END("test_stk500");
}
