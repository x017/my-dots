#ifndef DWM_SCREENSHOT_GEOMETRY_H
#define DWM_SCREENSHOT_GEOMETRY_H
typedef struct { int x, y, w, h; } ScreenshotRect;
/* Normalize a drag from either direction and clamp it to the X root window. */
static ScreenshotRect screenshot_rect(int ax, int ay, int bx, int by, int width, int height) {
  ScreenshotRect r = {0, 0, 0, 0};
  int t;
  if (width < 1 || height < 1) return r;
  if (ax < 0) ax = 0;
  if (bx < 0) bx = 0;
  if (ay < 0) ay = 0;
  if (by < 0) by = 0;
  if (ax > width) ax = width;
  if (bx > width) bx = width;
  if (ay > height) ay = height;
  if (by > height) by = height;
  if (ax > bx) { t = ax; ax = bx; bx = t; }
  if (ay > by) { t = ay; ay = by; by = t; }
  r.x = ax; r.y = ay; r.w = bx - ax; r.h = by - ay;
  return r;
}
#endif
