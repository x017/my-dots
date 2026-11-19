/* Small RGB PNG encoder; zlib is loaded at runtime, like Imlib2 elsewhere. */
#ifndef DWM_SCREENSHOT_PNG_H
#define DWM_SCREENSHOT_PNG_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <dlfcn.h>

static uint32_t screenshot_crc(uint32_t crc, const unsigned char *p, size_t n) {
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return crc;
}
static int screenshot_u32(FILE *f, uint32_t n) {
  unsigned char b[4] = {n >> 24, n >> 16, n >> 8, n};
  return fwrite(b, 1, 4, f) == 4;
}
static int screenshot_chunk(FILE *f, const char type[4], const unsigned char *data, uint32_t n) {
  uint32_t crc = screenshot_crc(0xffffffffU, (const unsigned char *)type, 4);
  crc = screenshot_crc(crc, data, n) ^ 0xffffffffU;
  return screenshot_u32(f, n) && fwrite(type, 1, 4, f) == 4 &&
         (!n || fwrite(data, 1, n, f) == n) && screenshot_u32(f, crc);
}
static int screenshot_png(FILE *f, const unsigned char *rgb, unsigned int w, unsigned int h) {
  typedef unsigned long (*Bound)(unsigned long);
  typedef int (*Compress)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int);
  static const unsigned char sig[] = {137,80,78,71,13,10,26,10};
  unsigned char ihdr[13] = {0}, *raw = NULL, *packed = NULL;
  size_t stride, rawlen; unsigned long packedlen;
  void *lib = NULL; Bound bound; Compress compress; int ok = 0;
  if (!f || !rgb || !w || !h) return 0;
  stride = (size_t)w * 3;
  if ((size_t)h > SIZE_MAX / (stride + 1)) return 0;
  rawlen = (stride + 1) * h;
  if (rawlen > ULONG_MAX) return 0;
  lib = dlopen("libz.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!lib) return 0;
  *(void **)(&bound) = dlsym(lib, "compressBound");
  *(void **)(&compress) = dlsym(lib, "compress2");
  if (!bound || !compress) goto out;
  packedlen = bound((unsigned long)rawlen);
  if (packedlen > UINT32_MAX) goto out;
  raw = malloc(rawlen); packed = malloc(packedlen);
  if (!raw || !packed) goto out;
  for (unsigned int y = 0; y < h; y++) {
    raw[(stride + 1) * y] = 0; /* PNG filter: none */
    memcpy(raw + (stride + 1) * y + 1, rgb + stride * y, stride);
  }
  if (compress(packed, &packedlen, raw, (unsigned long)rawlen, 6) != 0 || packedlen > UINT32_MAX) goto out;
  ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
  ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
  ihdr[8] = 8; ihdr[9] = 2; /* RGB, 8 bits */
  ok = fwrite(sig, 1, sizeof sig, f) == sizeof sig && screenshot_chunk(f, "IHDR", ihdr, 13) &&
       screenshot_chunk(f, "IDAT", packed, (uint32_t)packedlen) && screenshot_chunk(f, "IEND", NULL, 0);
out:
  free(raw); free(packed); if (lib) dlclose(lib); return ok;
}
#endif
