#define _DEFAULT_SOURCE
/* Exercises the real screenshot helpers: drag geometry, channel scaling and
 * the PNG encoder (round-tripped through zlib's own inflate). */
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../screenshot_geometry.h"
#include "../screenshot_pixel.h"
#include "../screenshot_png.h"

static unsigned char *readall(const char *path, size_t *out) {
  FILE *f = fopen(path, "rb"); long n; unsigned char *buf;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) < 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0) { fclose(f); return NULL; }
  buf = malloc((size_t)n);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
  fclose(f); *out = buf ? (size_t)n : 0; return buf;
}

int main(void) {
  /* Drag geometry: forward, reversed, clipped and degenerate selections. */
  { ScreenshotRect r = screenshot_rect(10, 20, 110, 220, 1000, 800);
    assert(r.x == 10 && r.y == 20 && r.w == 100 && r.h == 200);
    r = screenshot_rect(110, 220, 10, 20, 1000, 800);
    assert(r.x == 10 && r.y == 20 && r.w == 100 && r.h == 200);
    r = screenshot_rect(-40, -5, 2000, 900, 1000, 800);
    assert(r.x == 0 && r.y == 0 && r.w == 1000 && r.h == 800);
    r = screenshot_rect(300, 300, 300, 300, 1000, 800);
    assert(r.w == 0 && r.h == 0);
    r = screenshot_rect(0, 0, 500, 500, 0, 800);
    assert(r.w == 0 && r.h == 0); }
  /* Channel scaling: 8-bit, 16-bit and missing masks. */
  assert(screenshot_component(0x00ff0000UL, 0x00ff0000UL) == 255);
  assert(screenshot_component(0x00000080UL, 0x000000ffUL) == 128);
  assert(screenshot_component(0xffc00000UL, 0xffc00000UL) == 255);
  assert(screenshot_component(0x80000000UL, 0xffc00000UL) == 128);
  assert(screenshot_component(0x0000ffc0UL, 0x0000ffc0UL) == 255);
  assert(screenshot_component(0xffffffffUL, 0x00000000UL) == 0);
  /* Known-answer test for the chunk CRC routine. */
  assert((screenshot_crc(0xffffffffU, (const unsigned char *)"123456789", 9) ^ 0xffffffffU) == 0xcbf43926U);

  { /* Encode 3x2 pixels and verify the stream byte for byte. */
    const unsigned int w = 3, h = 2;
    const unsigned char px[18] = { 0,0,0, 255,0,0, 0,255,0, 0,0,255, 128,64,32, 255,255,255 };
    char path[] = "/tmp/dwm-screenshot-test-XXXXXX";
    unsigned char *file, *raw = NULL, *idat = NULL, *inflated = NULL;
    size_t n = 0, idatlen = 0, expectlen = (w * 3 + 1) * h, at = 8;
    unsigned long inflatedlen;
    int (*uncompress)(unsigned char *, unsigned long *, const unsigned char *, unsigned long);
    void *zlib;
    int fd = mkstemp(path); FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    assert(f && screenshot_png(f, px, w, h) && fclose(f) == 0);
    assert((file = readall(path, &n)) && n > 8 && !memcmp(file, "\x89PNG\r\n\x1a\n", 8));
    zlib = dlopen("libz.so.1", RTLD_LAZY | RTLD_LOCAL);
    assert(zlib); *(void **)(&uncompress) = dlsym(zlib, "uncompress"); assert(uncompress);
    while (at + 8 <= n) {
      uint32_t len = (uint32_t)file[at] << 24 | (uint32_t)file[at + 1] << 16 |
                     (uint32_t)file[at + 2] << 8 | file[at + 3], crc;
      unsigned char *body = file + at + 4;
      if (at + 12 + len > n) break;
      crc = ((uint32_t)body[len + 4] << 24) | ((uint32_t)body[len + 5] << 16) |
            ((uint32_t)body[len + 6] << 8) | body[len + 7];
      assert((screenshot_crc(screenshot_crc(0xffffffffU, body, 4), body + 4, len) ^ 0xffffffffU) == crc);
      if (!memcmp(body, "IHDR", 4)) {
        assert(len == 13 && body[12] == 8 && body[13] == 2);
        assert(((uint32_t)body[4] << 24 | (uint32_t)body[5] << 16 | (uint32_t)body[6] << 8 | body[7]) == w);
        assert(((uint32_t)body[8] << 24 | (uint32_t)body[9] << 16 | (uint32_t)body[10] << 8 | body[11]) == h);
      } else if (!memcmp(body, "IDAT", 4)) {
        unsigned char *grown = realloc(idat, idatlen + len);
        assert(grown); idat = grown; memcpy(idat + idatlen, body + 4, len); idatlen += len;
      } else if (!memcmp(body, "IEND", 4)) assert(len == 0);
      at += 12 + len;
    }
    assert(idat && idatlen);
    inflated = malloc(expectlen * 8);
    inflatedlen = expectlen * 8;
    assert(uncompress(inflated, &inflatedlen, idat, idatlen) == 0 && inflatedlen == expectlen);
    raw = malloc(expectlen);
    for (unsigned int y = 0; y < h; y++) {
      raw[y * (w * 3 + 1)] = 0; /* filter none */
      memcpy(raw + y * (w * 3 + 1) + 1, px + y * w * 3, w * 3);
    }
    assert(!memcmp(inflated, raw, expectlen));
    free(file); free(raw); free(idat); free(inflated); dlclose(zlib); unlink(path);
    /* A zero-sized capture must be rejected instead of writing a bad file. */
    f = fopen("/tmp/dwm-screenshot-empty.png", "wb");
    assert(f && !screenshot_png(f, px, 0, 2) && !fclose(f));
    unlink("/tmp/dwm-screenshot-empty.png");
  }
  puts("screenshot drag geometry, channel scaling and PNG encoding passed");
  return 0;
}
