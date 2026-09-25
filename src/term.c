#include "term.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static int device_error;

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
    while (!stop) {
        int n = a->p->read(a->p, buf, sizeof buf, 200);
        if (n < 0) {
            fprintf(stderr, "\n[nanoterm] lost the board (unplugged?)\n");
            device_error = 1;
            stop = 1;
        } else if (n > 0) {
            emit(a->o, buf, n);
        }
    }
    return NULL;
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
    int stdin_open = 1;
    char line[512];
    size_t len = 0;

    while (!stop) {
        if (o->duration > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec >= o->duration) break;
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

        if (o->log) {
            char ts[32] = "";
            if (o->timestamps) stamp(ts, sizeof ts);
            fprintf(o->log, "%s> %.*s\n", ts, (int)len, line);
            fflush(o->log);
        }
        size_t eol = strlen(o->eol);
        memcpy(line + len, o->eol, eol); // fits: len <= sizeof line - 2, eol <= 2
        if (p->write(p, (unsigned char *)line, (int)(len + eol)) < 0)
            fprintf(stderr, "[nanoterm] write failed\n");
        len = 0;
    }

    stop = 1;
    pthread_join(t, NULL);
    return device_error ? -1 : 0;
}
