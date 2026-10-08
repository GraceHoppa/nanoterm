#include "term.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "betaflight.h"

static volatile sig_atomic_t stop;
static int device_error;

// Betaflight's CLI prompt is "\r\n# ". Comment lines in `diff` output also
// start with "# ", so the prompt only counts once the output has gone quiet.
#define PROMPT_QUIET_MS 400
static _Atomic long last_rx_ms;
static _Atomic int at_prompt;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int prompt_ready(void) {
    return at_prompt && now_ms() - last_rx_ms >= PROMPT_QUIET_MS;
}

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

// "12:34:56.789 "
static void stamp(char *buf, size_t len) {
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    size_t n = strftime(buf, len, "%H:%M:%S", &tm);
    snprintf(buf + n, len - n, ".%03ld ", (long)(tv.tv_usec / 1000));
}

// Print to the output and, if logging, the log -- same bytes to both.
static void out2(const term_opts *o, const char *s, size_t n) {
    fwrite(s, 1, n, o->out);
    if (o->log) fwrite(s, 1, n, o->log);
}

static void emit(const term_opts *o, const unsigned char *data, int n) {
    static int at_line_start = 1, col;
    char ts[32], hx[4];
    for (int i = 0; i < n; i++) {
        if (at_line_start && o->timestamps) {
            stamp(ts, sizeof ts);
            out2(o, ts, strlen(ts));
        }
        if (o->hex) {
            // Row of 16; a newline byte also ends the row, so line-based
            // protocols still read line by line.
            snprintf(hx, sizeof hx, "%02x ", data[i]);
            out2(o, hx, 3);
            if (++col == 16 || data[i] == '\n') {
                out2(o, "\n", 1);
                col = 0;
            }
            at_line_start = col == 0;
        } else {
            out2(o, (const char *)&data[i], 1);
            at_line_start = data[i] == '\n';
        }
    }
    fflush(o->out);
    if (o->log) fflush(o->log);
}

typedef struct {
    port *p;
    const term_opts *o;
} reader_args;

static void *reader(void *arg) {
    reader_args *a = arg;
    unsigned char buf[64];
    char tail[3] = { 0 }; // the last three bytes received
    while (!stop) {
        int n = a->p->read(a->p, buf, sizeof buf, 200);
        if (n < 0) {
            fprintf(stderr, "\n[nanoterm] lost the board (unplugged?)\n");
            device_error = 1;
            stop = 1;
        } else if (n > 0) {
            emit(a->o, buf, n);
            for (int i = 0; i < n; i++) {
                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = (char)buf[i];
            }
            at_prompt = !memcmp(tail, "\n# ", 3);
            last_rx_ms = now_ms();
        }
    }
    return NULL;
}

// Log a line and send it with the line ending. line needs 2 spare bytes.
static void send_line(port *p, const term_opts *o, char *line, size_t len) {
    if (o->log) {
        char ts[32] = "";
        if (o->timestamps) stamp(ts, sizeof ts);
        fprintf(o->log, "%s> %.*s\n", ts, (int)len, line);
        fflush(o->log);
    }
    size_t eol = strlen(o->eol);
    memcpy(line + len, o->eol, eol); // fits: callers leave 2 bytes, eol <= 2
    if (p->write(p, (unsigned char *)line, (int)(len + eol)) < 0)
        fprintf(stderr, "[nanoterm] write failed\n");
}

// Betaflight guard for one typed line: 1 = send it, 2 = send the held
// line instead, 0 = send nothing. With allow_writes a changing command is
// held in pending until the next line says "y".
static int bf_allow(const term_opts *o, const char *line, size_t len, char *pending, size_t *pendlen) {
    if (*pendlen) {
        size_t n = *pendlen;
        if (len == 1 && (line[0] == 'y' || line[0] == 'Y')) return 2;
        *pendlen = 0;
        fprintf(stderr, "[nanoterm] not sent: %.*s\n", (int)n, pending);
        return 0;
    }
    if (bf_classify(line, len) != BF_WRITE) return 1;
    if (!o->allow_writes) {
        fprintf(stderr, "[nanoterm] read-only: '%.*s' would change the flight controller, not sent "
                        "(start with -W to allow changes)\n", (int)len, line);
        return 0;
    }
    memcpy(pending, line, len);
    *pendlen = len;
    fprintf(stderr, "[nanoterm] '%.*s' changes the flight controller. Props off? "
                    "Type y + Enter to send it, anything else drops it.\n", (int)len, line);
    return 0;
}

int term_run(port *p, const term_opts *o) {
    // No SA_RESTART: a signal must interrupt poll() below.
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    stop = 0;
    device_error = 0;
    pthread_t t;
    reader_args a = { p, o };
    pthread_create(&t, NULL, reader, &a);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int stdin_open = !o->oneshot;
    char line[512], pending[512];
    size_t len = 0, pendlen = 0;

    // One-shot stages: 0 = waiting for the first prompt, 1 = command sent,
    // waiting for the prompt to come back.
    int stage = 0;
    if (o->betaflight) {
        at_prompt = 0;
        last_rx_ms = now_ms();
        if (p->write(p, (const unsigned char *)"#", 1) < 0) stop = 1; // '#' enters the CLI
    }

    while (!stop) {
        if (o->duration > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec >= o->duration) {
                if (o->oneshot) {
                    fprintf(stderr, "\n[nanoterm] no CLI prompt within %d s -- is it a Betaflight board?\n",
                            o->duration);
                    device_error = 1;
                }
                break;
            }
        }
        if (o->oneshot && prompt_ready()) {
            if (stage == 1) break;
            len = strlen(o->oneshot);
            if (len > sizeof line - 2) len = sizeof line - 2;
            memcpy(line, o->oneshot, len);
            at_prompt = 0;
            send_line(p, o, line, len);
            len = 0;
            stage = 1;
        }
        // Poll instead of blocking in fgets(): keeps the loop able to
        // notice stop/duration, and EOF on stdin doesn't end the session
        // (so `nanoterm > log` and `nanoterm < /dev/null` keep monitoring).
        struct pollfd pfd = { stdin_open ? STDIN_FILENO : -1, POLLIN, 0 };
        int r = poll(&pfd, 1, 200);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0 || !(pfd.revents & (POLLIN | POLLHUP))) continue;

        char c;
        ssize_t got = read(STDIN_FILENO, &c, 1);
        if (got <= 0) {
            stdin_open = 0;
            continue;
        }
        if (c == '\r') continue; // CRLF input: the '\n' ends the line
        if (c != '\n') {
            // Keep 2 bytes free for the longest line ending ("\r\n").
            if (len < sizeof line - 2) line[len++] = c;
            continue;
        }

        if (o->betaflight) {
            int ok = bf_allow(o, line, len, pending, &pendlen);
            if (ok == 2) {
                memcpy(line, pending, pendlen);
                len = pendlen;
                pendlen = 0;
            } else if (!ok) {
                len = 0;
                continue;
            }
        }
        send_line(p, o, line, len);
        len = 0;
    }

    stop = 1;
    pthread_join(t, NULL);
    return device_error ? -1 : 0;
}
