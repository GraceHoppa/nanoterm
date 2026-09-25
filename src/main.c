// nanoterm: serial terminal + sketch uploader for CH340-based Arduino
// clones, talking to the USB chip directly through libusb. That is what lets
// it run unrooted in Termux on Android (via termux-usb), as well as on Linux.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ch340.h"
#include "ihex.h"
#include "stk500.h"
#include "term.h"

#define NANOTERM_VERSION "0.2.0"

static int quiet;

#define info(...)                                  \
    do {                                           \
        if (!quiet) fprintf(stderr, __VA_ARGS__);  \
    } while (0)

static void usage(FILE *f) {
    fputs("usage: nanoterm [options] [FD]\n"
          "\n"
          "Serial terminal for a CH340 Arduino board (Nano/Uno clones). Type a\n"
          "line + Enter to send it; Ctrl+C quits.\n"
          "\n"
          "terminal:\n"
          "  -b BAUD   speed, must match Serial.begin() in the sketch (default 9600)\n"
          "  -t        timestamp each received line\n"
          "  -x        show received bytes as hex\n"
          "  -e EOL    line ending sent after what you type: lf (default), crlf, cr, none\n"
          "  -l FILE   also append the session to FILE (sent lines marked \"> \")\n"
          "  -n        don't reset the board on connect (attach to a running sketch)\n"
          "  -d SECS   exit after SECS seconds\n"
          "\n"
          "upload:\n"
          "  -u HEX    upload a compiled sketch (Intel HEX, e.g. from\n"
          "            arduino-cli compile --output-dir build), verify it, exit\n"
          "  -U BAUD   bootloader speed: 115200 (default, current Nano/Uno),\n"
          "            57600 for Nanos with the \"old bootloader\"\n"
          "  -m        after uploading, stay connected as a terminal\n"
          "  -v        log the upload byte by byte (for debugging a board)\n"
          "\n"
          "  -q        quiet: no [nanoterm] status lines\n"
          "  -V        print version\n"
          "  -h        this help\n"
          "\n"
          "FD is an open USB file descriptor, as passed by `termux-usb -e`;\n"
          "without one the first CH340 on the bus is used (Linux).\n",
          f);
}

// Parse a positive integer or exit with a usage error.
static unsigned num_arg(const char *s, char flag) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || !*s || v == 0 || v > 4000000) {
        fprintf(stderr, "nanoterm: -%c: '%s' is not a valid number\n", flag, s);
        exit(2);
    }
    return (unsigned)v;
}

// The nt wrapper can't pass arguments through `termux-usb -e`, which takes a
// single command word, so it writes them NUL-separated to a file named by
// NANOTERM_ARGFILE. Rebuild argv from it.
static char **argfile_argv(const char *path, char *argv0, int *argc) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "nanoterm: %s: %s\n", path, strerror(errno));
        exit(2);
    }
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    static char *av[128];
    int ac = 0;
    av[ac++] = argv0;
    for (size_t i = 0; i < n && ac < 127; i += strlen(buf + i) + 1) av[ac++] = buf + i;
    av[ac] = NULL;
    *argc = ac;
    return av;
}

// Set while the progress line is drawn but not yet ended with a newline.
static int progress_open;

static void progress(void *user, int verifying, int page, int pages) {
    (void)user;
    fprintf(stderr, "\r[nanoterm] %s %3d%%", verifying ? "verifying" : "writing  ", page * 100 / pages);
    progress_open = page != pages;
    if (!progress_open) fputc('\n', stderr);
}

// termux-usb -e exits 0 whatever its command returned, so nt reads the real
// exit code from the file named by NANOTERM_STATUS.
static int finish(int code) {
    const char *status = getenv("NANOTERM_STATUS");
    FILE *f = status && *status ? fopen(status, "w") : NULL;
    if (f) {
        fprintf(f, "%d\n", code);
        fclose(f);
    }
    return code;
}

int main(int argc, char **argv) {
    unsigned baud = 9600, upload_baud = 115200;
    const char *hexpath = NULL, *logpath = NULL;
    int monitor_after = 0, no_reset = 0, verbose = 0, opt;
    term_opts to = { .eol = "\n" };

    const char *argfile = getenv("NANOTERM_ARGFILE");
    if (argfile && *argfile) argv = argfile_argv(argfile, argv[0], &argc);

    while ((opt = getopt(argc, argv, "b:txe:l:nd:u:U:mvqVh")) != -1) {
        switch (opt) {
        case 'b': baud = num_arg(optarg, 'b'); break;
        case 't': to.timestamps = 1; break;
        case 'x': to.hex = 1; break;
        case 'e':
            if (!strcmp(optarg, "lf")) to.eol = "\n";
            else if (!strcmp(optarg, "crlf")) to.eol = "\r\n";
            else if (!strcmp(optarg, "cr")) to.eol = "\r";
            else if (!strcmp(optarg, "none")) to.eol = "";
            else {
                fprintf(stderr, "nanoterm: -e: use lf, crlf, cr or none\n");
                return finish(2);
            }
            break;
        case 'l': logpath = optarg; break;
        case 'n': no_reset = 1; break;
        case 'd': to.duration = (int)num_arg(optarg, 'd'); break;
        case 'u': hexpath = optarg; break;
        case 'U': upload_baud = num_arg(optarg, 'U'); break;
        case 'm': monitor_after = 1; break;
        case 'v': verbose = 1; break;
        case 'q': quiet = 1; break;
        case 'V': puts("nanoterm " NANOTERM_VERSION); return finish(0);
        case 'h': usage(stdout); return finish(0);
        default: usage(stderr); return finish(2);
        }
    }
    if (!ch340_divisor(baud) || !ch340_divisor(upload_baud)) {
        fprintf(stderr, "nanoterm: baud rate out of range (46..3000000)\n");
        return finish(2);
    }

    int fd = -1;
    const char *fdenv = getenv("TERMUX_USB_FD");
    if (fdenv && *fdenv) fd = atoi(fdenv);
    if (optind < argc) {
        char *end;
        long v = strtol(argv[optind], &end, 10);
        if (*end || v < 0 || optind + 1 != argc) {
            usage(stderr);
            return finish(2);
        }
        fd = (int)v;
    }
    // Under termux-usb, stdout is captured until exit; only stderr is live.
    to.out = fd >= 0 ? stderr : stdout;

    // Read the sketch before touching the board: a bad file shouldn't
    // reset a running Nano.
    static unsigned char img[STK500_FLASH_MAX];
    int imgsize = 0;
    char err[256];
    if (hexpath) {
        imgsize = ihex_load(hexpath, img, STK500_FLASH_MAX, err, sizeof err);
        if (imgsize < 0) {
            fprintf(stderr, "[nanoterm] %s: %s\n", hexpath, err);
            // arduino-cli writes this one right next to the right file.
            if (strstr(hexpath, "with_bootloader"))
                fprintf(stderr, "[nanoterm] that file includes the bootloader -- upload the plain "
                                "<sketch>.ino.hex instead\n");
            return finish(1);
        }
    }
    if (logpath && !(to.log = fopen(logpath, "a"))) {
        fprintf(stderr, "[nanoterm] %s: %s\n", logpath, strerror(errno));
        return finish(1);
    }

    ch340 c;
    if (ch340_open(&c, fd, err, sizeof err) < 0) {
        fprintf(stderr, "[nanoterm] %s\n", err);
        return finish(1);
    }
    port *p = &c.base;
    int code = 0;

    if (hexpath) {
        if (ch340_configure(&c, upload_baud, 0) < 0) {
            fprintf(stderr, "[nanoterm] can't configure the USB serial chip\n");
            code = 1;
            goto out;
        }
        info("[nanoterm] uploading %s (%d bytes) at %u baud\n", hexpath, imgsize, upload_baud);
        stk500_opts so = { .progress = quiet || verbose ? NULL : progress, .trace = verbose ? stderr : NULL };
        if (stk500_upload(p, img, imgsize, &so, err, sizeof err) < 0) {
            fprintf(stderr, "%s[nanoterm] upload failed: %s\n", progress_open ? "\n" : "", err);
            code = 1;
            goto out;
        }
        info("[nanoterm] done: %d bytes written and verified\n", imgsize);
        if (!monitor_after) goto out;
        // The sketch was just started by the bootloader; no reset needed.
        if (ch340_set_baud(&c, baud) < 0) {
            code = 1;
            goto out;
        }
    } else {
        if (ch340_configure(&c, baud, 0) < 0) {
            fprintf(stderr, "[nanoterm] can't configure the USB serial chip\n");
            code = 1;
            goto out;
        }
        if (!no_reset) board_reset(p);
    }

    info("[nanoterm] %u baud%s%s%s%s -- type to send, Ctrl+C to quit\n", baud,
         to.timestamps ? ", timestamps" : "", to.hex ? ", hex" : "",
         to.log ? ", logging to " : "", to.log ? logpath : "");
    if (term_run(p, &to) < 0) code = 1;

out:
    ch340_close(&c);
    if (to.log) fclose(to.log);
    return finish(code);
}
