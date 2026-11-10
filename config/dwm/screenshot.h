/* Native monitor picker and area selector. Capture/PNG encoding runs in a
 * child with its own X connection so the window manager stays responsive. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "screenshot_geometry.h"
#include "screenshot_pixel.h"
#include "screenshot_png.h"

static int
screenshot_dir(char *out, size_t size)
{
  const char *override = getenv("DWM_SCREENSHOT_DIR"), *home = getenv("HOME");
  char parent[PATH_MAX]; struct stat st; int n;
  if (override && *override) {
    if (*override != '/') return 0;
    n = snprintf(out, size, "%s", override);
  } else {
    if (!home || *home != '/') return 0;
    n = snprintf(parent, sizeof parent, "%s/Pictures", home);
    if (n < 0 || (size_t)n >= sizeof parent || (mkdir(parent, 0700) < 0 && errno != EEXIST)) return 0;
    n = snprintf(out, size, "%s/Screenshots", parent);
  }
  if (n < 0 || (size_t)n >= size || (mkdir(out, 0700) < 0 && errno != EEXIST)) return 0;
  return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

static void
screenshot_save(ScreenshotRect r)
{
  Display *display; XImage *image = NULL; unsigned char *rgb = NULL;
  char dir[PATH_MAX], path[PATH_MAX], stamp[32], message[PATH_MAX + 100];
  struct timespec delay = {0, 150000000}; struct tm tm; time_t now;
  FILE *file = NULL; int fd = -1, ok = 0, n;
  const char *error = "Could not capture the selected area.";
  close(ConnectionNumber(dpy));
  /* Give a running compositor time to remove the picker and selection frame. */
  nanosleep(&delay, NULL);
  display = XOpenDisplay(NULL);
  if (!display) { error = "Could not open the X display."; goto done; }
  {
    int scr = DefaultScreen(display), w = DisplayWidth(display, scr), h = DisplayHeight(display, scr);
    if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x > w - r.w || r.y > h - r.h) {
      error = "The display geometry changed; try again."; goto done;
    }
    image = XGetImage(display, RootWindow(display, scr), r.x, r.y, (unsigned int)r.w, (unsigned int)r.h, AllPlanes, ZPixmap);
  }
  if (!image || !image->red_mask || !image->green_mask || !image->blue_mask) {
    error = "Could not read RGB pixels from the X display."; goto done;
  }
  if ((size_t)r.w > SIZE_MAX / 3 / (size_t)r.h) { error = "Screenshot is too large."; goto done; }
  rgb = malloc((size_t)r.w * (size_t)r.h * 3);
  if (!rgb) { error = "Not enough memory for the screenshot."; goto done; }
  if (image->format == ZPixmap && image->bits_per_pixel == 32) {
    /* Read the buffer directly; a full-screen XGetPixel loop is too slow. */
    const unsigned char *data = (const unsigned char *)image->data;
    size_t pitch = (size_t)image->bytes_per_line;
    for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
      uint32_t pixel; unsigned char *at = rgb + ((size_t)y * r.w + x) * 3;
      memcpy(&pixel, data + (size_t)y * pitch + (size_t)x * 4, 4);
      at[0] = screenshot_component(pixel, image->red_mask);
      at[1] = screenshot_component(pixel, image->green_mask);
      at[2] = screenshot_component(pixel, image->blue_mask);
    }
  } else {
    for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
      unsigned long pixel = XGetPixel(image, x, y);
      unsigned char *at = rgb + ((size_t)y * r.w + x) * 3;
      at[0] = screenshot_component(pixel, image->red_mask);
      at[1] = screenshot_component(pixel, image->green_mask);
      at[2] = screenshot_component(pixel, image->blue_mask);
    }
  }
  if (!screenshot_dir(dir, sizeof dir)) { error = "Could not create the screenshot directory."; goto done; }
  now = time(NULL);
  if (!localtime_r(&now, &tm) || !strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm)) {
    error = "Could not create a screenshot filename."; goto done;
  }
  for (int i = 0; i < 100; i++) {
    n = snprintf(path, sizeof path, "%s/Screenshot_%s_%ld_%d.png", dir, stamp, (long)getpid(), i);
    if (n < 0 || (size_t)n >= sizeof path) { error = "Screenshot path is too long."; goto done; }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) { error = "Could not create the screenshot file."; goto done; }
  file = fdopen(fd, "wb");
  if (!file) { close(fd); unlink(path); error = "Could not open the screenshot file."; goto done; }
  ok = screenshot_png(file, rgb, (unsigned int)r.w, (unsigned int)r.h);
  if (fclose(file) != 0) ok = 0;
  file = NULL;
  if (!ok) { unlink(path); error = "PNG encoding failed; check that zlib is installed and disk space is available."; goto done; }
  fprintf(stderr, "dwm screenshot: saved %s\n", path);
done:
  free(rgb); if (image) XDestroyImage(image); if (display) XCloseDisplay(display);
  if (ok) snprintf(message, sizeof message, "Saved to %s", path);
  else { snprintf(message, sizeof message, "%s", error); fprintf(stderr, "dwm screenshot: %s\n", error); }
  execlp("notify-send", "notify-send", "-a", "dwm", ok ? "Screenshot saved" : "Screenshot failed", message, (char *)NULL);
  _exit(ok ? 0 : 1);
}

static void
screenshot_start(ScreenshotRect r)
{
  pid_t child;
  if (r.w < 1 || r.h < 1) return;
  XSync(dpy, False); /* all picker and selection windows have been destroyed */
  child = fork();
  if (child == 0) { setsid(); signal(SIGCHLD, SIG_DFL); screenshot_save(r); }
  if (child < 0) fprintf(stderr, "dwm screenshot: could not start capture: %s\n", strerror(errno));
}

static void
screenshotmonitor(const Arg *arg)
{
  ScreenshotRect rects[64]; char labels[64][128]; char *items[64];
  Monitor *m; int count = 0, selected;
  (void)arg;
  for (m = mons; m && count < (int)LENGTH(rects); m = m->next) {
    rects[count] = (ScreenshotRect){m->mx, m->my, m->mw, m->mh};
    snprintf(labels[count], sizeof labels[count], "DISPLAY %d   %d x %d   @ %d,%d%s",
             m->num + 1, m->mw, m->mh, m->mx, m->my, m == selmon ? "   / CURRENT" : "");
    items[count] = labels[count]; count++;
  }
  selected = displaypick("S C R E E N S H O T  /  CHOOSE DISPLAY", items, count);
  if (selected >= 0 && selected < count) screenshot_start(rects[selected]);
}

static void
screenshotarea(const Arg *arg)
{
  Window edge[4] = {None, None, None, None}; XSetWindowAttributes wa;
  XColor blue, exact; unsigned long pixel; XEvent ev; Cursor cross;
  ScreenshotRect r = {0}; int done = 0, dragging = 0, drawn = 0, accept = 0, ax = 0, ay = 0;
  (void)arg;
  cross = XCreateFontCursor(dpy, XC_crosshair);
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) { XFreeCursor(dpy, cross); return; }
  if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                   GrabModeAsync, GrabModeAsync, None, cross, CurrentTime) != GrabSuccess) {
    XUngrabKeyboard(dpy, CurrentTime); XFreeCursor(dpy, cross); return;
  }
  pixel = WhitePixel(dpy, screen);
  if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "#7fa8c9", &blue, &exact)) pixel = blue.pixel;
  wa.override_redirect = True; wa.background_pixel = pixel; wa.event_mask = 0;
  for (int i = 0; i < 4; i++)
    edge[i] = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, DefaultDepth(dpy, screen), InputOutput,
                            DefaultVisual(dpy, screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  while (!done && running) {
    XNextEvent(dpy, &ev);
    if (ev.type == KeyPress && XLookupKeysym(&ev.xkey, 0) == XK_Escape) done = 1;
    else if (ev.type == ButtonPress && ev.xbutton.button == Button1 && !dragging) {
      ax = ev.xbutton.x_root; ay = ev.xbutton.y_root; dragging = 1;
    } else if ((ev.type == MotionNotify && dragging) || (ev.type == ButtonRelease && dragging && ev.xbutton.button == Button1)) {
      int x = ev.type == MotionNotify ? ev.xmotion.x_root : ev.xbutton.x_root;
      int y = ev.type == MotionNotify ? ev.xmotion.y_root : ev.xbutton.y_root;
      r = screenshot_rect(ax, ay, x, y, sw, sh);
      if (r.w >= 2 && r.h >= 2) {
        XMoveResizeWindow(dpy, edge[0], r.x, r.y, r.w, 2);
        XMoveResizeWindow(dpy, edge[1], r.x, r.y + r.h - 2, r.w, 2);
        XMoveResizeWindow(dpy, edge[2], r.x, r.y, 2, r.h);
        XMoveResizeWindow(dpy, edge[3], r.x + r.w - 2, r.y, 2, r.h);
        if (!drawn) { for (int i = 0; i < 4; i++) XMapRaised(dpy, edge[i]); drawn = 1; }
      } else if (drawn) { for (int i = 0; i < 4; i++) XUnmapWindow(dpy, edge[i]); drawn = 0; }
      if (ev.type == ButtonRelease) { accept = r.w >= 2 && r.h >= 2; done = 1; }
    } else if (ev.type == ButtonPress && ev.xbutton.button == Button3) done = 1;
    else if (ev.type == ConfigureNotify && ev.xconfigure.window == root) { done = 1; configurenotify(&ev); }
    else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify && ev.type != FocusIn &&
             ev.type != KeyPress && ev.type != KeyRelease && ev.type != ButtonPress && ev.type != ButtonRelease &&
             ev.type != MotionNotify && handler[ev.type]) handler[ev.type](&ev);
  }
  for (int i = 0; i < 4; i++) if (edge[i]) XDestroyWindow(dpy, edge[i]);
  XUngrabPointer(dpy, CurrentTime); XUngrabKeyboard(dpy, CurrentTime); XFreeCursor(dpy, cross);
  if (accept) screenshot_start(r);
}
