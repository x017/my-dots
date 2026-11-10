#ifndef DWM_SCREENSHOT_PIXEL_H
#define DWM_SCREENSHOT_PIXEL_H
#include <stdint.h>
/* Scale one color channel of an X11 ZPixmap pixel to 8 bits. The mask gives
 * both the bit position and the channel depth (16- and 10-bit visuals exist). */
static unsigned char screenshot_component(unsigned long pixel, unsigned long mask) {
  unsigned int shift = 0;
  uint64_t max, value;
  if (!mask) return 0;
  while (!(mask & 1)) { mask >>= 1; shift++; }
  max = mask;
  value = ((uint64_t)pixel >> shift) & max;
  return (unsigned char)((value * 255 + max / 2) / max);
}
#endif
