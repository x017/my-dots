/* Desktop Entry Exec parsing, following the freedesktop specification. Free of
 * CUI so it can be exercised directly by the test suite. */
#ifndef DWM_LAUNCHER_EXEC_H
#define DWM_LAUNCHER_EXEC_H
#include <stddef.h>

#define LAUNCHER_ARGV 32
#define LAUNCHER_ARG 256

static int launcher_arg_push(char argv[][LAUNCHER_ARG], int *count, int max,
                             const char *value, size_t length) {
    if (*count >= max || length >= LAUNCHER_ARG) return -1;
    if (length) {
        char *dst = argv[*count];
        for (size_t i = 0; i < length; i++) dst[i] = value[i];
        dst[length] = '\0';
        (*count)++;
    }
    return 0;
}

/* Expand one Exec= value into argv: unquoted whitespace separates arguments,
 * double quotes group them, backslash escapes resolve, and % field codes are
 * expanded (%i, %c, %k, %%) or dropped (%f %F %u %U and friends). Returns the
 * argument count, or -1 when the line is malformed or too long. */
static int launcher_exec_argv(const char *exec, const char *name, const char *icon,
                              const char *desktop_file, char argv[][LAUNCHER_ARG], int max) {
    char pending[LAUNCHER_ARG];
    size_t length = 0;
    int count = 0, quoted = 0;
    const char *p;
    if (!exec) return -1;
    for (p = exec; *p; p++) {
        char c = *p;
        if (c == '"' ) { quoted = !quoted; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (launcher_arg_push(argv, &count, max, pending, length) < 0) return -1;
            length = 0;
            continue;
        }
        if (c == '\\' && p[1]) {
            char e = *++p;
            switch (e) {
                case 's': c = ' '; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'e': c = 27; break;
                case 'b': c = 8; break;
                case 'f': c = 12; break;
                case '\\': case '"': case '\'': c = e; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    int value = 0, digits = 0;
                    while (digits < 3 && p[0] >= '0' && p[0] <= '7') {
                        value = value * 8 + (p[0] - '0'); p++; digits++;
                    }
                    p--;
                    c = (char)value;
                    break;
                }
                default: c = e; break;   /* reserved escapes keep the character */
            }
        } else if (c == '%' && p[1]) {
            char code = *++p;
            const char *value = NULL;
            switch (code) {
                case '%': c = '%'; break;
                case 'i':
                    /* %i becomes two arguments, and nothing when Icon is unset. */
                    if (icon && *icon) {
                        if (launcher_arg_push(argv, &count, max, pending, length) < 0) return -1;
                        length = 0;
                        if (launcher_arg_push(argv, &count, max, "--icon", 6) < 0) return -1;
                        value = icon;
                    } else continue;
                    break;
                case 'c': value = name && *name ? name : NULL; if (!value) continue; break;
                case 'k': value = desktop_file && *desktop_file ? desktop_file : NULL; if (!value) continue; break;
                default: continue;       /* %f %F %u %U %d %D %n %N %v %m are dropped */
            }
            if (value) {
                if (launcher_arg_push(argv, &count, max, pending, length) < 0) return -1;
                length = 0;
                if (launcher_arg_push(argv, &count, max, value, strlen(value)) < 0) return -1;
                continue;
            }
        }
        if (length + 1 >= LAUNCHER_ARG) return -1;
        pending[length++] = c;
    }
    if (quoted) return -1;
    if (launcher_arg_push(argv, &count, max, pending, length) < 0) return -1;
    return count;
}
#endif
