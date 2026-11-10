/* Native monitor picker and area selector. Capture, PNG encoding and clipboard
 * ownership run in a child with its own X connection so the window manager
 * stays responsive. */
#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "screenshot_dir.h"
#include "screenshot_geometry.h"
#include "screenshot_pixel.h"
#include "screenshot_png.h"

/* Largest property payload that fits one X request; anything bigger needs the
 * INCR protocol, which is what xclip uses for images. */
static size_t
screenshot_limit(Display *display)
{
  long max = XMaxRequestSize(display);
  size_t limit = max > 64 ? (size_t)max * 4 : 262144;
  if (limit > 262144) limit = 262144;
  if (limit < 16384) limit = 16384;
  return limit;
}

/* Wait for the next event, giving up after timeout_ms. Returns 0 on timeout. */
static int
screenshot_event(Display *display, int timeout_ms, XEvent *ev)
{
  while (!XPending(display)) {
    fd_set set; struct timeval tv;
    FD_ZERO(&set); FD_SET(ConnectionNumber(display), &set);
    tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(ConnectionNumber(display) + 1, &set, NULL, NULL, &tv) <= 0) return 0;
  }
  XNextEvent(display, ev);
  return 1;
}

static void
screenshot_property(Display *display, Window requestor, Atom property, Atom type,
                    const unsigned char *data, size_t length)
{
  size_t limit = screenshot_limit(display), chunk = limit / 2, sent = 0;
  if (length + 64 <= limit) {
    XChangeProperty(display, requestor, property, type, 8, PropModeReplace, data, (int)length);
    return;
  }
  {
    Atom incr = XInternAtom(display, "INCR", False);
    unsigned long total = (unsigned long)length;
    XSelectInput(display, requestor, PropertyChangeMask);
    XChangeProperty(display, requestor, property, incr, 32, PropModeReplace, (unsigned char *)&total, 1);
    for (;;) {
      XEvent ev; size_t n;
      /* The requestor deletes the property to ask for the next chunk. */
      do {
        if (!screenshot_event(display, 10000, &ev)) return;
      } while (ev.type != PropertyNotify || ev.xproperty.window != requestor ||
               ev.xproperty.atom != property || ev.xproperty.state != PropertyDelete);
      n = length - sent < chunk ? length - sent : chunk;
      XChangeProperty(display, requestor, property, type, 8, PropModeReplace, data + sent, (int)n);
      sent += n;
      if (!n) break;
    }
  }
}

/* Own CLIPBOARD and serve the PNG until another client takes the selection
 * over, then exit. This keeps dwm's event loop free of selection handling. */
static void
screenshot_clipboard(Display *display, const unsigned char *png, size_t length,
                     const char *path, const char *uri)
{
  Atom clipboard, targets, png_atom, xpng, urilist, utf8;
  XSetWindowAttributes wa; Window win; XEvent ev;
  time_t deadline = time(NULL) + 1800;

  clipboard = XInternAtom(display, "CLIPBOARD", False);
  targets = XInternAtom(display, "TARGETS", False);
  png_atom = XInternAtom(display, "image/png", False);
  xpng = XInternAtom(display, "image/x-png", False);
  urilist = XInternAtom(display, "text/uri-list", False);
  utf8 = XInternAtom(display, "UTF8_STRING", False);
  wa.override_redirect = True; wa.event_mask = PropertyChangeMask;
  win = XCreateWindow(display, DefaultRootWindow(display), -1, -1, 1, 1, 0, 0, InputOnly,
                      CopyFromParent, CWOverrideRedirect | CWEventMask, &wa);
  XSetSelectionOwner(display, clipboard, win, CurrentTime);
  if (XGetSelectionOwner(display, clipboard) != win) { XDestroyWindow(display, win); return; }
  while (time(NULL) < deadline && screenshot_event(display, 1000, &ev)) {
    XSelectionRequestEvent *req; XSelectionEvent reply; Atom property;
    if (ev.type == SelectionClear) break;
    if (ev.type != SelectionRequest) continue;
    req = &ev.xselectionrequest;
    property = req->property ? req->property : req->target;
    memset(&reply, 0, sizeof reply);
    reply.type = SelectionNotify; reply.display = display; reply.requestor = req->requestor;
    reply.selection = req->selection; reply.target = req->target; reply.time = req->time;
    if (req->target == targets) {
      Atom list[5]; int count = 0;
      list[count++] = targets; list[count++] = png_atom; list[count++] = xpng;
      if (uri[0]) { list[count++] = urilist; list[count++] = utf8; }
      XChangeProperty(display, req->requestor, property, XA_ATOM, 32, PropModeReplace,
                      (unsigned char *)list, count);
      reply.property = property;
    } else if (req->target == png_atom || req->target == xpng) {
      screenshot_property(display, req->requestor, property, png_atom, png, length);
      reply.property = property;
    } else if (uri[0] && req->target == urilist) {
      screenshot_property(display, req->requestor, property, urilist,
                          (const unsigned char *)uri, strlen(uri));
      reply.property = property;
    } else if (uri[0] && (req->target == utf8 || req->target == XA_STRING)) {
      screenshot_property(display, req->requestor, property, req->target,
                          (const unsigned char *)path, strlen(path));
      reply.property = property;
    }
    XSendEvent(display, req->requestor, False, 0, (XEvent *)&reply);
    XFlush(display);
  }
  XDestroyWindow(display, win);
}

static void
screenshot_notify(int saved, const char *message)
{
  signal(SIGCHLD, SIG_IGN);
  if (fork() == 0) {
    execlp("notify-send", "notify-send", "-a", "dwm", saved ? "Screenshot saved" : "Screenshot failed",
           message, (char *)NULL);
    _exit(127);
  }
}

static void
screenshot_save(ScreenshotRect r)
{
  Display *display = NULL; XImage *image = NULL; unsigned char *rgb = NULL;
  char *png = NULL, dir[PATH_MAX], path[PATH_MAX], uri[PATH_MAX + 16], stamp[32];
  char message[PATH_MAX + 160];
  struct timespec delay = {0, 150000000}; struct tm tm; time_t now;
  size_t pnglen = 0; FILE *stream = NULL, *file = NULL;
  int fd = -1, captured = 0, saved = 0, n;
  const char *error = "Could not capture the selected area.";

  path[0] = uri[0] = '\0';
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
    image = XGetImage(display, RootWindow(display, scr), r.x, r.y, (unsigned int)r.w,
                      (unsigned int)r.h, AllPlanes, ZPixmap);
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
  /* Encode once into memory: the same bytes go to the file and the clipboard. */
  stream = open_memstream(&png, &pnglen);
  if (!stream) { error = "Not enough memory for the PNG."; goto done; }
  if (!screenshot_png(stream, rgb, (unsigned int)r.w, (unsigned int)r.h) || fclose(stream)) {
    stream = NULL; error = "PNG encoding failed; check that zlib is installed and disk space is available."; goto done;
  }
  stream = NULL;
  if (!png || !pnglen) { error = "PNG encoding produced no data."; goto done; }
  captured = 1;
  if (!screenshot_dir(dir, sizeof dir)) { error = "Could not create the screenshot directory."; goto done; }
  now = time(NULL);
  if (!localtime_r(&now, &tm) || !strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm)) {
    error = "Could not create a screenshot filename."; goto done;
  }
  for (n = 0; n < 100; n++) {
    if (snprintf(path, sizeof path, "%s/Screenshot_%s_%ld_%d.png", dir, stamp, (long)getpid(), n) >= (int)sizeof path) {
      error = "Screenshot path is too long."; goto done;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) { path[0] = '\0'; error = "Could not create the screenshot file."; goto done; }
  file = fdopen(fd, "wb");
  if (!file) { close(fd); unlink(path); path[0] = '\0'; error = "Could not open the screenshot file."; goto done; }
  saved = fwrite(png, 1, pnglen, file) == pnglen && !fclose(file);
  file = NULL;
  if (!saved) { unlink(path); path[0] = '\0'; error = "Could not write the screenshot file; check disk space."; goto done; }
  snprintf(uri, sizeof uri, "file://%s", path);
done:
  if (stream) fclose(stream);
  if (file) fclose(file);
  free(rgb);
  if (image) XDestroyImage(image);
  if (captured) {
    if (saved) snprintf(message, sizeof message, "Saved to %s and copied to the clipboard", path);
    else snprintf(message, sizeof message, "%s The image is on the clipboard.", error);
    fprintf(stderr, "dwm screenshot: %s\n", message);
    screenshot_notify(saved, message);
    screenshot_clipboard(display, (unsigned char *)png, pnglen, path, uri);
  } else {
    fprintf(stderr, "dwm screenshot: %s\n", error);
    screenshot_notify(0, error);
  }
  free(png);
  if (display) XCloseDisplay(display);
  _exit(saved ? 0 : 1);
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

/* Monitor picker drawn like the Alt+` switcher: each card previews that
 * display live, so the shot can be aimed before it is taken. */
static int
screenshotpick(Monitor **chosen)
{
  Monitor *m, *origin = selmon, **screens;
  Client *c;
  Window *wins, popup;
  Damage *damage;
  XEvent ev;
  XSetWindowAttributes wa;
  Drw *d;
  Clr *head, *normal, *selected, *blue;
  const char *font[] = {"monospace:size=10", "Symbols Nerd Font:size=11"};
  const char *hc[] = {"#8a949e", "#090b0d", "#2d3035"};
  const char *nc[] = {"#adbac7", "#121417", "#1e2025"};
  const char *sc[] = {"#e9eff6", "#181a1e", "#e9eff6"};
  const char *bc[] = {"#7fa8c9", "#090b0d", "#1e2025"};
  unsigned int nm = 0, nw = 0, i, j, first, slots, active = 0;
  int width, height, pad = 16, rh, cardw, cardh, previewh;
  int done = 0, accept = 0, havepreview, ce, cr, de, dr, re, rr, major, minor;
  int (*oldhandler)(Display *, XErrorEvent *);
  char text[512];
  KeySym key;

  *chosen = NULL;
  for (m = mons; m; m = m->next) {
    nm++;
    for (c = m->clients; c; c = c->next) if (ISVISIBLE(c)) nw++;
  }
  if (nm < 2) return 0;
  screens = ecalloc(nm, sizeof(Monitor *));
  wins = ecalloc(MAX(1, nw), sizeof(Window));
  damage = ecalloc(MAX(1, nw), sizeof(Damage));
  for (m = mons, i = 0, j = 0; m; m = m->next, i++) {
    screens[i] = m;
    if (m == origin) active = i;
    for (c = m->clients; c; c = c->next) if (ISVISIBLE(c)) wins[j++] = c->win;
  }
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
    free(screens); free(wins); free(damage); return 0;
  }
  havepreview = XCompositeQueryExtension(dpy, &ce, &cr) &&
                XDamageQueryExtension(dpy, &de, &dr) &&
                XRenderQueryExtension(dpy, &re, &rr) &&
                XCompositeQueryVersion(dpy, &major, &minor) && (major > 0 || minor >= 2);
  if (havepreview) {
    XSync(dpy, False); oldhandler = XSetErrorHandler(xerrordummy);
    for (i = 0; i < nw; i++) {
      XCompositeRedirectWindow(dpy, wins[i], CompositeRedirectAutomatic);
      damage[i] = XDamageCreate(dpy, wins[i], XDamageReportNonEmpty);
    }
    XSync(dpy, False); XSetErrorHandler(oldhandler);
  }
  width = MAX(1, MIN(1100, origin->mw - 20));
  height = MAX(1, MIN(460, origin->mh - 20));
  rh = MAX(28, bh + 4);
  slots = MIN(nm, width >= 850 ? 3U : (width >= 450 ? 2U : 1U));
  cardw = MAX(1, (width - (int)(slots + 1) * pad) / (int)slots);
  cardh = MAX(1, height - 2 * rh - 2 * pad);
  previewh = MAX(1, cardh - rh - 8);
  wa.override_redirect = True; wa.background_pixel = BlackPixel(dpy, screen);
  wa.event_mask = ExposureMask | ButtonPressMask;
  popup = XCreateWindow(dpy, root, origin->mx + (origin->mw - width) / 2,
                        origin->my + (origin->mh - height) / 2, width, height, 1,
                        DefaultDepth(dpy, screen), CopyFromParent, DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  d = drw_create(dpy, screen, root, width, height);
  drw_fontset_create(d, font, LENGTH(font));
  head = drw_scm_create(d, hc, 3); normal = drw_scm_create(d, nc, 3);
  selected = drw_scm_create(d, sc, 3); blue = drw_scm_create(d, bc, 3);
  XSetWindowBorder(dpy, popup, head[ColBorder].pixel); XMapRaised(dpy, popup);
  while (!done && running) {
    first = (active / slots) * slots;
    drw_setscheme(d, head); drw_rect(d, 0, 0, width, height, 1, 1);
    drw_text(d, pad, pad, MAX(1, width - 2 * pad), rh, 4, "S C R E E N S H O T  /  CHOOSE DISPLAY", 0);
    drw_setscheme(d, head);
    drw_rect(d, pad, pad + rh, MAX(1, width - 2 * pad), 1, 1, 0);
    for (i = 0; i < slots && first + i < nm; i++) {
      int x = pad + i * (cardw + pad), y = pad + rh + 12;
      int px = x + 8, py = y + rh + 4, pw = MAX(1, cardw - 16);
      m = screens[first + i];
      drw_setscheme(d, first + i == active ? selected : normal);
      drw_rect(d, x, y, cardw, cardh, 1, 1);
      drw_rect(d, x, y, cardw, cardh, 0, 0);
      snprintf(text, sizeof text, "DISPLAY %u  ·  %d x %d%s", m->num + 1, m->mw, m->mh,
               m == origin ? "  ·  CURRENT" : "");
      drw_text(d, x + 1, y + 1, MAX(1, cardw - 2), rh, 8, text, 0);
      drw_setscheme(d, head); drw_rect(d, px, py, pw, previewh, 1, 1);
      /* dwm's own window geometries keep the preview spatially accurate. */
      for (j = 0; j < nw; j++) {
        int wx, wy, ww, wh;
        c = wintoclient(wins[j]);
        if (!c || c->mon != m || !ISVISIBLE(c)) continue;
        wx = MAX(0, MIN(pw - 1, (int)((long)(c->x - m->mx) * pw / MAX(1, m->mw))));
        wy = MAX(0, MIN(previewh - 1, (int)((long)(c->y - m->my) * previewh / MAX(1, m->mh))));
        ww = MAX(1, MIN(pw - wx, (int)((long)c->w * pw / MAX(1, m->mw))));
        wh = MAX(1, MIN(previewh - wy, (int)((long)c->h * previewh / MAX(1, m->mh))));
        drw_setscheme(d, normal); drw_rect(d, px + wx, py + wy, ww, wh, 1, 1);
        if (havepreview) switcherpreview(d, c->win, px + wx, py + wy, ww, wh);
        drw_setscheme(d, c == m->sel ? selected : normal);
        drw_rect(d, px + wx, py + wy, ww, wh, 0, 0);
      }
      drw_setscheme(d, first + i == active ? selected : normal);
      drw_text(d, x + 1, y + cardh - rh, MAX(1, cardw - 2), rh, 8,
               first + i == active ? "ENTER captures this display" : "", 0);
    }
    drw_setscheme(d, head);
    drw_text(d, pad, height - pad - rh, MAX(1, width - 2 * pad), rh, 4,
             "ARROWS / TAB switch   ENTER or CLICK capture   ESC cancel", 0);
    drw_map(d, popup, 0, 0, width, height);
    XNextEvent(dpy, &ev);
    if (havepreview && ev.type == de + XDamageNotify) {
      XDamageNotifyEvent *event = (XDamageNotifyEvent *)&ev;
      XSync(dpy, False); oldhandler = XSetErrorHandler(xerrordummy);
      XDamageSubtract(dpy, event->damage, None, None);
      while (XCheckTypedEvent(dpy, de + XDamageNotify, &ev)) {
        event = (XDamageNotifyEvent *)&ev;
        XDamageSubtract(dpy, event->damage, None, None);
      }
      XSync(dpy, False); XSetErrorHandler(oldhandler);
    } else if (ev.type == KeyPress) {
      key = XLookupKeysym(&ev.xkey, 0);
      if (key == XK_Escape) done = 1;
      else if (key == XK_Return || key == XK_KP_Enter) done = accept = 1;
      else if (key == XK_Tab || key == XK_Right || key == XK_Left || key == XK_Down || key == XK_Up) {
        int reverse = key == XK_Left || key == XK_Up || (ev.xkey.state & ShiftMask);
        active = (active + (reverse ? nm - 1 : 1)) % nm;
      }
    } else if (ev.type == ButtonPress && ev.xbutton.window == popup) {
      int slot = (ev.xbutton.x - pad) / MAX(1, cardw + pad);
      if (ev.xbutton.button == Button4) active = (active + nm - 1) % nm;
      else if (ev.xbutton.button == Button5) active = (active + 1) % nm;
      else if (ev.xbutton.button == Button1 && slot >= 0 && first + (unsigned int)slot < nm) {
        active = first + (unsigned int)slot; done = accept = 1;
      }
    } else if (ev.type == ConfigureNotify && ev.xconfigure.window == root) {
      /* Monitor pointers can change during a hotplug. Close before dispatch. */
      done = 1;
      configurenotify(&ev);
    } else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify &&
               ev.type != FocusIn && handler[ev.type]) handler[ev.type](&ev);
  }
  XDestroyWindow(dpy, popup); XUngrabKeyboard(dpy, CurrentTime);
  if (havepreview) {
    XSync(dpy, False); oldhandler = XSetErrorHandler(xerrordummy);
    for (i = 0; i < nw; i++) {
      if (damage[i]) XDamageDestroy(dpy, damage[i]);
      XCompositeUnredirectWindow(dpy, wins[i], CompositeRedirectAutomatic);
    }
    XSync(dpy, False); XSetErrorHandler(oldhandler);
    /* Extension events must not reach dwm's core-event dispatch table. */
    while (XCheckTypedEvent(dpy, de + XDamageNotify, &ev)) { }
  }
  drw_scm_free(d, normal, 3); drw_scm_free(d, selected, 3); drw_scm_free(d, head, 3);
  drw_scm_free(d, blue, 3); drw_free(d);
  if (accept && screens[active]) *chosen = screens[active];
  free(screens); free(wins); free(damage);
  return accept;
}

static void
screenshotmonitor(const Arg *arg)
{
  Monitor *picked = NULL, *m;
  unsigned int nm = 0;
  (void)arg;
  for (m = mons; m; m = m->next) nm++;
  if (!nm) return;
  /* A single display needs no chooser: capture it straight away. */
  if (nm == 1) picked = mons;
  else if (!screenshotpick(&picked) || !picked) return; /* Escape cancels */
  { ScreenshotRect r = {picked->mx, picked->my, picked->mw, picked->mh}; screenshot_start(r); }
}

static void
screenshotarea(const Arg *arg)
{
  Window edge[4] = {None, None, None, None}; XSetWindowAttributes wa;
  XColor blue, exact; unsigned long pixel; XEvent ev; Cursor cross;
  ScreenshotRect r = {0, 0, 0, 0};
  int done = 0, dragging = 0, drawn = 0, accept = 0, ax = 0, ay = 0, i;
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
  for (i = 0; i < 4; i++)
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
        if (!drawn) { for (i = 0; i < 4; i++) XMapRaised(dpy, edge[i]); drawn = 1; }
      } else if (drawn) { for (i = 0; i < 4; i++) XUnmapWindow(dpy, edge[i]); drawn = 0; }
      if (ev.type == ButtonRelease) { accept = r.w >= 2 && r.h >= 2; done = 1; }
    } else if (ev.type == ButtonPress && ev.xbutton.button == Button3) done = 1;
    else if (ev.type == ConfigureNotify && ev.xconfigure.window == root) { done = 1; configurenotify(&ev); }
    else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify && ev.type != FocusIn &&
             ev.type != KeyPress && ev.type != KeyRelease && ev.type != ButtonPress && ev.type != ButtonRelease &&
             ev.type != MotionNotify && handler[ev.type]) handler[ev.type](&ev);
  }
  for (i = 0; i < 4; i++) if (edge[i]) XDestroyWindow(dpy, edge[i]);
  XUngrabPointer(dpy, CurrentTime); XUngrabKeyboard(dpy, CurrentTime); XFreeCursor(dpy, cross);
  if (accept) screenshot_start(r);
}
