/* FriBidi converts logical UTF-8 into shaped visual text for Xft's simple
 * string API. Optional at runtime; leave text unchanged when unavailable. */
#ifndef DWM_PERSIAN_TEXT_H
#define DWM_PERSIAN_TEXT_H
#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *persian_visual_text(const char *text) {
    typedef unsigned char (*Log2Vis)(const uint32_t *, int, uint32_t *, uint32_t *, int *, int *, unsigned char *);
    static int tried;
    static void *library;
    static Log2Vis log2vis;
    size_t bytes = strlen(text), count = 0, outlen = 0;
    uint32_t *logical, *visual, base = 0x40; /* FRIBIDI_PAR_ON: first strong */
    char *out;
    int needs = 0;
    if (!bytes || bytes > INT_MAX || bytes > (SIZE_MAX / sizeof(uint32_t)) - 1 || bytes > (SIZE_MAX - 1) / 4) return NULL;
    /* Keep the common ASCII-only UI path allocation-free. */
    { const unsigned char *p = (const unsigned char *)text;
      while (*p && *p < 0x80) p++;
      if (!*p) return NULL; }
    logical = malloc((bytes + 1) * sizeof *logical);
    if (!logical) return NULL;
    for (size_t i = 0; i < bytes;) {
        unsigned char c = (unsigned char)text[i]; uint32_t cp; int n, valid = 1;
        if (c < 0x80) { cp = c; n = 1; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; n = 2; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; n = 3; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; n = 4; }
        else { valid = 0; cp = 0; n = 1; }
        if (i + n > bytes) valid = 0;
        if (valid) for (int j = 1; j < n; j++) {
            unsigned char d = (unsigned char)text[i+j];
            if ((d & 0xc0) != 0x80) { valid = 0; break; }
            cp = (cp << 6) | (d & 63);
        }
        if (!valid || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { free(logical); return NULL; }
        logical[count++] = cp; i += n;
        if ((cp >= 0x590 && cp <= 0x8ff) || (cp >= 0xfb1d && cp <= 0xfeff) ||
            (cp >= 0x200c && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
            (cp >= 0x2066 && cp <= 0x2069)) needs = 1;
    }
    if (!needs) { free(logical); return NULL; }
    if (!tried) {
        tried = 1; library = dlopen("libfribidi.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (library) *(void **)(&log2vis) = dlsym(library, "fribidi_log2vis");
    }
    if (!log2vis) { free(logical); return NULL; }
    visual = malloc((count + 1) * sizeof *visual);
    out = malloc(bytes * 4 + 1);
    if (!visual || !out || !log2vis(logical, (int)count, &base, visual, NULL, NULL, NULL)) {
        free(logical); free(visual); free(out); return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = visual[i];
        /* FriBidi uses FEFF as the filler for ligatures and controls. */
        if (cp == 0xfeff || (cp >= 0x200b && cp <= 0x200f) ||
            (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069)) continue;
        if (cp < 0x80) out[outlen++] = cp;
        else if (cp < 0x800) { out[outlen++] = 0xc0 | (cp >> 6); out[outlen++] = 0x80 | (cp & 63); }
        else if (cp < 0x10000) { out[outlen++] = 0xe0 | (cp >> 12); out[outlen++] = 0x80 | ((cp >> 6) & 63); out[outlen++] = 0x80 | (cp & 63); }
        else { out[outlen++] = 0xf0 | (cp >> 18); out[outlen++] = 0x80 | ((cp >> 12) & 63); out[outlen++] = 0x80 | ((cp >> 6) & 63); out[outlen++] = 0x80 | (cp & 63); }
    }
    out[outlen] = 0; free(logical); free(visual); return out;
}
#endif
