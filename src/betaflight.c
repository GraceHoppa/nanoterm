#include "betaflight.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

// Read-only whatever follows: `diff all`, `dump hardware`, `get gyro`...
static const char *const always_read[] = {
    "diff", "dump", "status", "version", "get", "help", "tasks",
};

// Read-only when typed alone (or followed by `show`/`list`); with
// arguments they change things -- `serial` lists ports, `serial 1 64 ...`
// reassigns one; `profile` shows the current one, `profile 2` switches.
static const char *const bare_read[] = {
    "adjrange", "aux",     "beacon",  "beeper",      "color",   "dma",
    "feature",  "led",     "map",     "mixer",       "mmix",    "mode_color",
    "name",     "profile", "rateprofile", "resource", "rxrange", "serial",
    "servo",    "smix",    "timer",   "vtx",         "vtxtable",
};

static int in(const char *const *list, size_t n, const char *word, size_t len) {
    for (size_t i = 0; i < n; i++)
        if (strlen(list[i]) == len && !strncasecmp(list[i], word, len)) return 1;
    return 0;
}

bf_kind bf_classify(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)line[i])) i++;
    while (len > i && isspace((unsigned char)line[len - 1])) len--;
    if (i == len) return BF_EMPTY;

    const char *cmd = line + i;
    size_t cmdlen = 0;
    while (i + cmdlen < len && !isspace((unsigned char)cmd[cmdlen])) cmdlen++;

    if (in(always_read, sizeof always_read / sizeof *always_read, cmd, cmdlen)) return BF_READ;
    if (!in(bare_read, sizeof bare_read / sizeof *bare_read, cmd, cmdlen)) return BF_WRITE;

    const char *rest = cmd + cmdlen;
    size_t restlen = len - (size_t)(rest - line);
    while (restlen && isspace((unsigned char)*rest)) rest++, restlen--;
    if (restlen == 0) return BF_READ;
    if ((restlen == 4 && !strncasecmp(rest, "show", 4)) || (restlen == 4 && !strncasecmp(rest, "list", 4)))
        return BF_READ;
    return BF_WRITE;
}
