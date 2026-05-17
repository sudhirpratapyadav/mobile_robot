// Minimal INI parser shared by the standalone driver (dyna_train.c) and
// any other C-side consumer. The Python training path reads the same file
// via PufferLib's own loader — this header just gives the C side parity so
// the standalone executable doesn't drift from dyna_train.ini.
//
// Supports:
//   * [section] headers
//   * key = value lines (whitespace insensitive)
//   * `#` or `;` line comments
//   * blank lines
//   * values parsed as double via strtod; ignored if non-numeric
//
// Not supported (intentionally):
//   * quoted strings, arrays, multi-line values
//   * include directives
//   * environment expansion
//
// Usage:
//   IniEntry entries[256];
//   int n = ini_load("dyna_train.ini", entries, 256);
//   double arena = ini_get_d(entries, n, "env", "arena_size", 20.0);
//   int max_steps = ini_get_i(entries, n, "env", "max_steps", 800);
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INI_MAX_STR 64

typedef struct {
    char section[INI_MAX_STR];
    char key[INI_MAX_STR];
    double value;
} IniEntry;

static inline char* ini__strip(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

// Returns number of numeric entries parsed (≥ 0), or -1 if the file can't
// be opened. Non-numeric values (strings) are silently skipped — only
// numbers go into the table.
static inline int ini_load(const char* path, IniEntry* entries, int max_entries) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    char section[INI_MAX_STR] = "";
    int n = 0;

    while (fgets(line, sizeof(line), f)) {
        // Drop comments at first '#' or ';' that isn't inside a value (we
        // don't support quoted strings, so a trailing comment is also OK).
        for (char* p = line; *p; p++) {
            if (*p == '#' || *p == ';') { *p = '\0'; break; }
        }
        char* s = ini__strip(line);
        if (!*s) continue;

        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close) continue;
            *close = '\0';
            char* name = ini__strip(s + 1);
            strncpy(section, name, INI_MAX_STR - 1);
            section[INI_MAX_STR - 1] = '\0';
            continue;
        }

        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = ini__strip(s);
        char* val = ini__strip(eq + 1);
        if (!*key || !*val) continue;

        char* end = NULL;
        double d = strtod(val, &end);
        if (end == val) continue;  // not a number — skip silently

        if (n >= max_entries) {
            fprintf(stderr, "ini_load: max_entries=%d exceeded; ignoring [%s] %s\n",
                    max_entries, section, key);
            continue;
        }
        strncpy(entries[n].section, section, INI_MAX_STR - 1);
        entries[n].section[INI_MAX_STR - 1] = '\0';
        strncpy(entries[n].key, key, INI_MAX_STR - 1);
        entries[n].key[INI_MAX_STR - 1] = '\0';
        entries[n].value = d;
        n++;
    }

    fclose(f);
    return n;
}

static inline double ini_get_d(const IniEntry* entries, int n,
                               const char* section, const char* key,
                               double fallback) {
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].section, section) == 0 &&
            strcmp(entries[i].key, key) == 0) {
            return entries[i].value;
        }
    }
    return fallback;
}

static inline int ini_get_i(const IniEntry* entries, int n,
                            const char* section, const char* key,
                            int fallback) {
    return (int)ini_get_d(entries, n, section, key, (double)fallback);
}

static inline float ini_get_f(const IniEntry* entries, int n,
                              const char* section, const char* key,
                              float fallback) {
    return (float)ini_get_d(entries, n, section, key, (double)fallback);
}
