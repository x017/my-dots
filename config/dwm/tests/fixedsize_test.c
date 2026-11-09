/* Include dwm to exercise real size-hint and event handling with mocked X11. */
#define main dwm_program_main
#include "../dwm.c"
#undef main
#include <assert.h>

static XSizeHints test_hints;
static int hints_present = 1, last_w, last_h, resize_calls;
Status XGetWMNormalHints(Display *d, Window w, XSizeHints *hints, long *supplied) {
  (void)d; (void)w; *supplied = 0; if (!hints_present) return 0; *hints = test_hints; return 1;
}
int XConfigureWindow(Display *d, Window w, unsigned int mask, XWindowChanges *v) {
  (void)d; (void)w; (void)mask; last_w = v->width; last_h = v->height; resize_calls++; return 0;
}
int XMoveResizeWindow(Display *d, Window w, int x, int y, unsigned int width, unsigned int height) {
  (void)d; (void)w; (void)x; (void)y; last_w = width; last_h = height; resize_calls++; return 0;
}
int XMoveWindow(Display *d, Window w, int x, int y) { (void)d; (void)w; (void)x; (void)y; return 0; }
Status XSendEvent(Display *d, Window w, Bool propagate, long mask, XEvent *e) { (void)d; (void)w; (void)propagate; (void)mask; (void)e; return 0; }
int XSync(Display *d, Bool discard) { (void)d; (void)discard; return 0; }
int XChangeProperty(Display *d, Window w, Atom p, Atom type, int format, int mode, const unsigned char *data, int n) {
  (void)d; (void)w; (void)p; (void)type; (void)format; (void)mode; (void)data; (void)n; return 0;
}
int XRaiseWindow(Display *d, Window w) { (void)d; (void)w; return 0; }

int main(void) {
  Layout layout = { "test", NULL }; Monitor m = {0}; Client c = {0}; XEvent e = {0};
  m.mx = m.wx = 100; m.my = m.wy = 50; m.mw = m.ww = 1200; m.mh = m.wh = 800;
  m.lt[0] = &layout; m.drawwithgaps = 1; m.tagset[0] = 1;
  c.mon = &m; c.win = 123; c.bw = 2; c.tags = 1; c.w = 1000; c.h = 700;
  m.clients = m.stack = &c; mons = selmon = &m; bh = 28; sw = 1400; sh = 900;
  test_hints.flags = PMinSize | PMaxSize;
  test_hints.min_width = test_hints.max_width = 640;
  test_hints.min_height = test_hints.max_height = 360;
  updatesizehints(&c); assert(c.isfixed);
  initialclientgeometry(&c);
  assert(c.w == 640 && c.h == 360 && c.x == 378 && c.y == 268);
  /* Normal resizable clients retain their requested dimensions. */
  { Client normal = {0}; normal.mon = &m; normal.bw = 2; normal.w = 500; normal.h = 300;
    initialclientgeometry(&normal); assert(normal.w == 500 && normal.h == 300); }
  /* Explicit fullscreen takes precedence over fixed dimensions at launch. */
  { Client full = {0}; full.mon = &m; full.isfullscreen = full.isfixed = 1;
    full.w = 1200; full.h = 800; full.minw = 640; full.minh = 360; full.x = 100; full.y = 50;
    initialclientgeometry(&full); assert(full.w == 1200 && full.h == 800 && full.x == 100 && full.oldstate == 1); }
  /* Application asks for another size after being mapped: retain fixed size. */
  c.isfloating = 1; e.xconfigurerequest.window = c.win;
  e.xconfigurerequest.value_mask = CWWidth | CWHeight;
  e.xconfigurerequest.width = 1200; e.xconfigurerequest.height = 700;
  configurerequest(&e); assert(c.w == 640 && c.h == 360 && last_w == 640 && last_h == 360);
  /* Non-fixed floating applications still control their own size. */
  c.isfixed = 0; configurerequest(&e); assert(c.w == 1200 && c.h == 700);
  /* Hints arriving after mapping turn a tiled client into a fixed floating window. */
  c.isfloating = 0; c.isfixed = 0; c.x = 100; c.y = 50; c.w = 1200; c.h = 700;
  memset(&e, 0, sizeof e); e.xproperty.window = c.win; e.xproperty.atom = XA_WM_NORMAL_HINTS; e.xproperty.state = PropertyNewValue;
  propertynotify(&e);
  assert(c.isfixed && c.isfloating && c.w == 640 && c.h == 360 && c.x == 378 && c.y == 268);
  assert(last_w == 640 && last_h == 360 && resize_calls);
  /* Explicit fullscreen remains fullscreen; its restore size follows updated hints. */
  c.isfullscreen = 1; c.isfloating = 1; c.oldstate = 0; c.w = 1200; c.h = 800;
  c.oldx = 200; c.oldy = 100; c.oldw = 1200; c.oldh = 700; c.oldbw = 2;
  test_hints.min_width = test_hints.max_width = 800;
  test_hints.min_height = test_hints.max_height = 450;
  propertynotify(&e); assert(c.isfullscreen && c.w == 1200 && c.h == 800 && c.oldstate == 1);
  setfullscreen(&c, 0); assert(!c.isfullscreen && c.isfloating && c.w == 800 && c.h == 450);
  /* Removing hints clears the fixed flag, without forcing a floating window tiled. */
  hints_present = 0; e.xproperty.state = PropertyDelete; propertynotify(&e);
  assert(!c.isfixed && c.isfloating);
  puts("fixed-size launch, configure, late hints, fullscreen and hint removal passed"); return 0;
}
