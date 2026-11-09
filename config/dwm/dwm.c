/* See LICENSE file for copyright and license details.
 *
 * dynamic window manager is designed like any other X client as well. It is
 * driven through handling X events. In contrast to other X clients, a window
 * manager selects for SubstructureRedirectMask on the root window, to receive
 * events about window (dis-)appearance. Only one X connection at a time is
 * allowed to select for this event mask.
 *
 * The event handlers of dwm are organized in an array which is accessed
 * whenever a new event has been fetched. This allows event dispatching
 * in O(1) time.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag. Clients are organized in a linked client
 * list on each monitor, the focus history is remembered through a stack list
 * on each monitor. Each client contains a bit array to indicate the tags of a
 * client.
 *
 * Keys and tagging rules are organized as arrays and defined in config.h.
 *
 * To understand everything else, start reading main().
 */
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <locale.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "util.h"
#ifdef DWM_CUI
#include <cui/cui.h>
#include <cui/cui_render.h>
#endif

/* macros */
#define BUTTONMASK (ButtonPressMask | ButtonReleaseMask)
#define CLEANMASK(mask)                                                        \
  (mask & ~(numlockmask | LockMask) &                                          \
   (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask |      \
    Mod5Mask))
#define INTERSECT(x, y, w, h, m)                                               \
  (MAX(0, MIN((x) + (w), (m)->wx + (m)->ww) - MAX((x), (m)->wx)) *             \
   MAX(0, MIN((y) + (h), (m)->wy + (m)->wh) - MAX((y), (m)->wy)))
#define ISVISIBLE(C) ((C->tags & C->mon->tagset[C->mon->seltags]))
#define MOUSEMASK (BUTTONMASK | PointerMotionMask)
#define WIDTH(X) ((X)->w + 2 * (X)->bw)
#define HEIGHT(X) ((X)->h + 2 * (X)->bw)
#define TAGMASK ((1 << LENGTH(tags)) - 1)
#define TEXTW(X) (drw_fontset_getwidth(drw, (X)) + lrpad)

#define GAP_TOGGLE 100
#define GAP_RESET 0

#define SYSTEM_TRAY_REQUEST_DOCK 0
/* XEMBED messages */
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_FOCUS_IN 4
#define XEMBED_MODALITY_ON 10
#define XEMBED_MAPPED (1 << 0)
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_WINDOW_DEACTIVATE 2
#define VERSION_MAJOR 0
#define VERSION_MINOR 0
#define XEMBED_EMBEDDED_VERSION (VERSION_MAJOR << 16) | VERSION_MINOR

/* enums */
enum { CurNormal, CurResize, CurMove, CurLast }; /* cursor */
enum { SchemeNorm, SchemeSel };                  /* color schemes */
enum {
  NetSupported,
  NetWMName,
  NetWMState,
  NetWMCheck,
  NetSystemTray,
  NetSystemTrayOP,
  NetSystemTrayOrientation,
  NetSystemTrayOrientationHorz,
  NetWMFullscreen,
  NetActiveWindow,
  NetWMWindowType,
  NetWMWindowTypeDialog,
  NetClientList,
  NetLast
}; /* EWMH atoms */
enum { Manager, Xembed, XembedInfo, XLast }; /* Xembed atoms */
enum {
  WMProtocols,
  WMDelete,
  WMState,
  WMTakeFocus,
  WMLast
}; /* default atoms */
enum {
  ClkTagBar,
  ClkLtSymbol,
  ClkStatusText,
  ClkWinTitle,
  ClkClientWin,
  ClkRootWin,
  ClkLast
}; /* clicks */

typedef union {
  int i;
  unsigned int ui;
  float f;
  const void *v;
} Arg;

typedef struct {
  unsigned int click;
  unsigned int mask;
  unsigned int button;
  void (*func)(const Arg *arg);
  const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;
struct Client {
  char name[256];
  float mina, maxa;
  int x, y, w, h;
  int oldx, oldy, oldw, oldh;
  int basew, baseh, incw, inch, maxw, maxh, minw, minh, hintsvalid;
  int bw, oldbw;
  unsigned int tags;
  int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen;
  Client *next;
  Client *snext;
  Monitor *mon;
  Window win;
};

typedef struct {
  unsigned int mod;
  KeySym keysym;
  void (*func)(const Arg *);
  const Arg arg;
} Key;

typedef struct {
  const char *symbol;
  void (*arrange)(Monitor *);
} Layout;

struct Monitor {
  char ltsymbol[16];
  float mfact;
  int nmaster;
  int num;
  int by;             /* bar geometry */
  int mx, my, mw, mh; /* screen size */
  int wx, wy, ww, wh; /* window area  */
  int gappx;          /* gaps between windows */
  int drawwithgaps;   /* toggle gaps */
  unsigned int seltags;
  unsigned int sellt;
  unsigned int tagset[2];
  int showbar;
  int topbar;
  Client *clients;
  Client *sel;
  Client *stack;
  Monitor *next;
  Window barwin;
#ifdef DWM_CUI
  int cuibar;
#endif
  const Layout *lt[2];
};

typedef struct {
  const char *class;
  const char *instance;
  const char *title;
  unsigned int tags;
  int isfloating;
  int monitor;
} Rule;

typedef struct Systray Systray;
struct Systray {
  Window win;
  Client *icons;
};

/* function declarations */
static void alttab(const Arg *arg);
static void wallpaperswitcher(const Arg *arg);
static void togglekeyboardgroup(const Arg *arg);
static void displaymodeswitcher(const Arg *arg);
static void dashboardnative(const Arg *arg);
#ifdef DWM_CUI
static void dashboardcui(const Arg *arg);
#endif
static void monitorswitcher(const Arg *arg);
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h,
                          int interact);
static void arrange(Monitor *m);
static void warp(const Client *c);
static void arrangemon(Monitor *m);
static void attach(Client *c);
static void removesystrayicon(Client *i);
static void resizebarwin(Monitor *m);
static void resizerequest(XEvent *e);
static void attachstack(Client *c);
static void buttonpress(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void cleanupmon(Monitor *mon);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(void);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static void detachstack(Client *c);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
static void drawbars(void);
static int drawstatusbar(Monitor *m, int bh, char *text);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focus(Client *c);
static void focusin(XEvent *e);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Atom getatomprop(Client *c, Atom prop);
static int getrootptr(int *x, int *y);
static long getstate(Window w);
static unsigned int getsystraywidth();
static int gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void grabbuttons(Client *c, int focused);
static void grabkeys(void);
static void incnmaster(const Arg *arg);
static void keypress(XEvent *e);
static void killclient(const Arg *arg);
static void closeclient(Client *c);
static void manage(Window w, XWindowAttributes *wa);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(Monitor *m);
static void motionnotify(XEvent *e);
static void movemouse(const Arg *arg);
static Client *nexttiled(Client *c);
static void pop(Client *c);
static void propertynotify(XEvent *e);
static void quit(const Arg *arg);
static Monitor *recttomon(int x, int y, int w, int h);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizemouse(const Arg *arg);
static void restack(Monitor *m);
static void run(void);
static void runautostart(void);
static void scan(void);
static int sendevent(Window w, Atom proto, int m, long d0, long d1, long d2,
                     long d3, long d4);

static void updatesystray(void);
static Systray *systray = NULL;
static Client *wintosystrayicon(Window w);
static void updatesystrayicongeom(Client *i, int w, int h);
static void focusdir(const Arg *arg);
static void updatesystrayiconstate(Client *i, XPropertyEvent *ev);
static void sendmon(Client *c, Monitor *m);
static void setclientstate(Client *c, long state);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int fullscreen);
static void setgaps(const Arg *arg);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setup(void);
static void seturgent(Client *c, int urg);
static void showhide(Client *c);
static void spawn(const Arg *arg);
static Monitor *systraytomon(Monitor *m);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglebar(const Arg *arg);
static void togglefloating(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updateclientlist(void);
static int updategeom(void);
static void updatenumlockmask(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatetitle(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void view(const Arg *arg);
static Client *wintoclient(Window w);
static Monitor *wintomon(Window w);
static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);
static void zoom(const Arg *arg);

/* variables */
static const char autostartblocksh[] = "autostart_blocking.sh";
static const char autostartsh[] = "autostart.sh";
static const char broken[] = "broken";
static char stext[1024];
static int screen;
static int sw, sh; /* X display screen geometry width, height */
static int bh;     /* bar height */
static int lrpad;  /* sum of left and right padding for text */
static int (*xerrorxlib)(Display *, XErrorEvent *);
#ifdef DWM_CUI
#define CUI_BAR_PAD 8
#define CUI_BAR_TAG_WIDTH 30
#define CUI_BAR_LAYOUT_WIDTH 48
static int cuibarwidth(Monitor *m);
static int cuibarstatuswidth(Monitor *m);
static cui_ctx *bar_ui;
static cui_sw_renderer *bar_renderer;
#endif
static unsigned int numlockmask = 0;
static Atom xkbrules;
static void (*handler[LASTEvent])(XEvent *) = {
    [ButtonPress] = buttonpress,
    [ClientMessage] = clientmessage,
    [ConfigureRequest] = configurerequest,
    [ConfigureNotify] = configurenotify,
    [DestroyNotify] = destroynotify,
    [EnterNotify] = enternotify,
    [Expose] = expose,
    [FocusIn] = focusin,
    [KeyPress] = keypress,
    [MappingNotify] = mappingnotify,
    [MapRequest] = maprequest,
    [MotionNotify] = motionnotify,
    [PropertyNotify] = propertynotify,
    [ResizeRequest] = resizerequest,
    [UnmapNotify] = unmapnotify};
static Atom wmatom[WMLast], netatom[NetLast], xatom[XLast];
static int running = 1;
static Cur *cursor[CurLast];
static Clr **scheme;
static Display *dpy;
static Drw *drw;
static Monitor *mons, *selmon;
static Window root, wmcheckwin;

/* configuration, allows nested code to access above variables */
#include "config.h"

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags {
  char limitexceeded[LENGTH(tags) > 31 ? -1 : 1];
};

/* function implementations */
static int switcher_xerror_seen;
static int
switcherxerror(Display *display, XErrorEvent *error)
{
  (void)display; (void)error;
  switcher_xerror_seen = 1;
  return 0;
}

/* Draw a scaled, live Composite backing pixmap into the switcher.  A client
 * can vanish or unmap at any point; keep X errors local to this operation. */
static int
switcherpreview(Drw *d, Window win, int x, int y, int w, int h)
{
  XWindowAttributes attr;
  XRenderPictFormat *srcformat, *dstformat;
  Pixmap pix;
  Picture src, dst;
  XTransform transform;
  int pw, ph, ok = 0;
  int (*oldhandler)(Display *, XErrorEvent *);

  if (w < 1 || h < 1) return 0;
  XSync(dpy, False);
  switcher_xerror_seen = 0;
  oldhandler = XSetErrorHandler(switcherxerror);
  if (XGetWindowAttributes(dpy, win, &attr) && attr.map_state == IsViewable &&
      attr.width > 0 && attr.height > 0 &&
      (srcformat = XRenderFindVisualFormat(dpy, attr.visual)) &&
      (dstformat = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)))) {
    pix = XCompositeNameWindowPixmap(dpy, win);
    src = XRenderCreatePicture(dpy, pix, srcformat, 0, NULL);
    dst = XRenderCreatePicture(dpy, d->drawable, dstformat, 0, NULL);
    pw = w; ph = (int)((double)attr.height * pw / attr.width);
    if (ph > h) { ph = h; pw = (int)((double)attr.width * ph / attr.height); }
    pw = MAX(1, pw); ph = MAX(1, ph);
    memset(&transform, 0, sizeof transform);
    transform.matrix[0][0] = XDoubleToFixed((double)attr.width / pw);
    transform.matrix[1][1] = XDoubleToFixed((double)attr.height / ph);
    transform.matrix[2][2] = XDoubleToFixed(1);
    XRenderSetPictureTransform(dpy, src, &transform);
    XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    XRenderComposite(dpy, PictOpOver, src, None, dst, 0, 0, 0, 0,
                     x + (w - pw) / 2, y + (h - ph) / 2, pw, ph);
    XRenderFreePicture(dpy, src); XRenderFreePicture(dpy, dst);
    XFreePixmap(dpy, pix);
    ok = 1;
  }
  XSync(dpy, False);
  XSetErrorHandler(oldhandler);
  return ok && !switcher_xerror_seen;
}

/* Switch the XKB group directly; unlike an external helper this does not
 * race an XKB grp:win_space_toggle binding for the same key. grabkeys()
 * lets XKB handle the shortcut when that option is already configured. */
void togglekeyboardgroup(const Arg *arg) {
  XkbStateRec state;
  (void)arg;
  if (XkbGetState(dpy, XkbUseCoreKbd, &state) == Success)
    XkbLockGroup(dpy, XkbUseCoreKbd, state.group == 0 ? 1 : 0);
}

#include "dashboard_backend.h"
#include "dashboard_native.h"

#ifdef DWM_CUI
/* Keep the CUI UI in the dwm executable, but run its event loop in a child:
 * the window manager must remain free to manage its floating window. */
extern int dashboard_cui_main(void);
static void
dashboardcui(const Arg *arg)
{
  (void)arg;
  if (fork() == 0) {
    close(ConnectionNumber(dpy));
    setsid();
    signal(SIGCHLD, SIG_DFL);
    _exit(dashboard_cui_main());
  }
}
#endif

/* A small native list picker used by the display mode switcher. */
static int
displaypick(const char *title, char **items, int count)
{
  Monitor *m = selmon;
  XSetWindowAttributes wa;
  XEvent ev;
  Window win;
  Drw *d;
  Clr *normal, *selected, *head;
  const char *font[] = {"monospace:size=10", "Symbols Nerd Font:size=11"};
  const char *nc[] = {"#adbac7", "#121417", "#1e2025"};
  const char *sc[] = {"#f2d49e", "#181a1e", "#f2d49e"};
  const char *hc[] = {"#8a949e", "#090b0d", "#2d3035"};
  int width, height, rh, visible, choice = 0, start, i, done = 0, result = -1;
  KeySym key;
  if (count <= 0) return -1;
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) return -1;
  width = MAX(1, MIN(580, m->mw - 20));
  rh = MAX(30, bh + 6);
  visible = MAX(1, MIN(count, MIN(10, MAX(1, (m->mh - 4 * rh - 20) / rh))));
  height = (visible + 3) * rh;
  wa.override_redirect = True; wa.background_pixel = BlackPixel(dpy, screen);
  wa.event_mask = ExposureMask | ButtonPressMask;
  win = XCreateWindow(dpy, root, m->mx + (m->mw - width) / 2, m->my + (m->mh - height) / 2,
                      width, height, 1, DefaultDepth(dpy, screen), CopyFromParent,
                      DefaultVisual(dpy, screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  d = drw_create(dpy, screen, root, width, height);
  drw_fontset_create(d, font, LENGTH(font));
  normal = drw_scm_create(d, nc, 3); selected = drw_scm_create(d, sc, 3); head = drw_scm_create(d, hc, 3);
  XSetWindowBorder(dpy, win, head[ColBorder].pixel); XMapRaised(dpy, win);
  while (!done && running) {
    start = choice >= visible ? choice - visible + 1 : 0;
    drw_setscheme(d, head); drw_rect(d, 0, 0, width, height, 1, 1);
    drw_text(d, 12, 6, MAX(1, width - 24), rh, 8, title, 0);
    drw_rect(d, 12, rh + 6, MAX(1, width - 24), 1, 1, 0);
    for (i = 0; i < visible; i++) {
      int index = start + i;
      drw_setscheme(d, index == choice ? selected : normal);
      drw_text(d, 12, 2 * rh + i * rh, MAX(1, width - 24), rh - 2, 10,
               index < count ? items[index] : "", 0);
    }
    drw_setscheme(d, head);
    drw_text(d, 12, height - rh, MAX(1, width - 24), rh, 8, "UP/DOWN choose   ENTER select   ESC cancel", 0);
    drw_map(d, win, 0, 0, width, height);
    XNextEvent(dpy, &ev);
    if (ev.type == KeyPress) {
      key = XLookupKeysym(&ev.xkey, 0);
      if (key == XK_Escape) done = 1;
      else if (key == XK_Down || key == XK_j) choice = (choice + 1) % count;
      else if (key == XK_Up || key == XK_k) choice = (choice + count - 1) % count;
      else if (key == XK_Return || key == XK_KP_Enter) { result = choice; done = 1; }
    } else if (ev.type == ButtonPress && ev.xbutton.window == win) {
      if (ev.xbutton.button == Button4) choice = (choice + count - 1) % count;
      else if (ev.xbutton.button == Button5) choice = (choice + 1) % count;
      else if (ev.xbutton.button == Button1 && ev.xbutton.y >= 2 * rh && ev.xbutton.y < (2 + visible) * rh) {
        i = start + (ev.xbutton.y - 2 * rh) / rh;
        if (i < count) { result = i; done = 1; }
      }
    } else if (ev.type == ConfigureNotify && ev.xconfigure.window == root) {
      done = 1; configurenotify(&ev);
    } else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify &&
               ev.type != FocusIn && ev.type != KeyRelease && handler[ev.type]) handler[ev.type](&ev);
  }
  XDestroyWindow(dpy, win); XUngrabKeyboard(dpy, CurrentTime);
  drw_scm_free(d, normal, 3); drw_scm_free(d, selected, 3); drw_scm_free(d, head, 3); drw_free(d);
  return result;
}

/* The same three-column format used by the original refreshmenu script. */
static int
displayconfigpath(char *dir, size_t dirsize, char *path, size_t pathsize)
{
  const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
  int n;
  if (base && *base == '/') n = snprintf(dir, dirsize, "%s", base);
  else if (home && *home) n = snprintf(dir, dirsize, "%s/.config", home);
  else return 0;
  if (n < 0 || (size_t)n >= dirsize) return 0;
  n = snprintf(path, pathsize, "%s/refreshrate.conf", dir);
  return n >= 0 && (size_t)n < pathsize;
}

static int
displayapply(const char *output, const char *mode, const char *rate)
{
  pid_t child, waited;
  int status;
  child = fork();
  if (child < 0) return 0;
  if (child == 0) {
    execlp("xrandr", "xrandr", "--output", output, "--mode", mode, "--rate", rate, (char *)NULL);
    _exit(127);
  }
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
displaysave(const char *output, const char *mode, const char *rate)
{
  char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX], name[256];
  char *line = NULL;
  size_t len = 0;
  FILE *in = NULL, *out;
  int fd, ok = 1, needs_newline = 0;
  if (!displayconfigpath(dir, sizeof dir, path, sizeof path)) return 0;
  if (mkdir(dir, 0700) < 0 && errno != EEXIST) return 0;
  in = fopen(path, "r");
  if (!in && errno != ENOENT) return 0;
  if (snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path) >= (int)sizeof tmp) {
    if (in) fclose(in);
    return 0;
  }
  fd = mkstemp(tmp);
  if (fd < 0) { if (in) fclose(in); return 0; }
  out = fdopen(fd, "w");
  if (!out) { close(fd); unlink(tmp); if (in) fclose(in); return 0; }
  if (in) {
    while (getline(&line, &len, in) >= 0) {
      if (sscanf(line, "%255s", name) == 1 && !strcmp(name, output)) continue;
      if (fputs(line, out) == EOF) { ok = 0; break; }
      needs_newline = line[0] && line[strlen(line) - 1] != '\n';
    }
    if (ferror(in)) ok = 0;
    if (fclose(in) != 0) ok = 0;
  }
  free(line);
  if (needs_newline && fputc('\n', out) == EOF) ok = 0;
  if (fprintf(out, "%s %s %s\n", output, mode, rate) < 0) ok = 0;
  if (fclose(out) != 0) ok = 0;
  if (ok && rename(tmp, path) == 0) return 1;
  unlink(tmp);
  return 0;
}

/* This mode runs as a one-shot command from autostart, not as a second WM. */
static int
restoredisplaymodes(void)
{
  char dir[PATH_MAX], path[PATH_MAX], *line = NULL;
  char output[256], mode[64], rate[64], extra;
  size_t len = 0;
  int ok = 1;
  FILE *fp;
  if (!displayconfigpath(dir, sizeof dir, path, sizeof path)) return 1;
  fp = fopen(path, "r");
  if (!fp) return errno == ENOENT ? 0 : 1;
  signal(SIGCHLD, SIG_DFL);
  while (getline(&line, &len, fp) >= 0) {
    if (sscanf(line, "%255s %63s %63s %c", output, mode, rate, &extra) != 3) continue;
    if (!displayapply(output, mode, rate)) {
      fprintf(stderr, "dwm: failed to restore %s %s %s\n", output, mode, rate);
      ok = 0;
    }
  }
  if (ferror(fp)) ok = 0;
  free(line); fclose(fp);
  return ok ? 0 : 1;
}

/* The popup and persistence live in dwm. xrandr remains the X11 mode backend. */
void displaymodeswitcher(const Arg *arg)
{
  FILE *fp;
  char line[4096], output[256], state[32], mode[64], rate[64], title[320];
  char **outputs = NULL, **modes = NULL, **rates = NULL, **labels = NULL;
  int nout = 0, nmode = 0, chosen, selectedmode, active = 0, i;
  (void)arg;
  if (!(fp = popen("LC_ALL=C xrandr --query 2>/dev/null", "r"))) return;
  while (fgets(line, sizeof line, fp)) {
    if (line[0] == ' ' || line[0] == '\t') continue;
    if (sscanf(line, "%255s %31s", output, state) == 2 && !strcmp(state, "connected")) {
      char **next = realloc(outputs, (nout + 1) * sizeof *outputs);
      if (!next) die("realloc:");
      outputs = next; outputs[nout++] = strdup(output);
      if (!outputs[nout - 1]) die("strdup:");
    }
  }
  pclose(fp);
  if (!nout) {
    char *notice[] = {"No connected displays found. Check xrandr."};
    displaypick("D I S P L A Y S", notice, 1);
    goto cleanup;
  }
  chosen = displaypick("D I S P L A Y S   /   select monitor", outputs, nout);
  if (chosen < 0 || !(fp = popen("LC_ALL=C xrandr --query 2>/dev/null", "r"))) goto cleanup;
  while (fgets(line, sizeof line, fp)) {
    char *token, *save, *end;
    if (line[0] != ' ' && line[0] != '\t') {
      active = sscanf(line, "%255s %31s", output, state) == 2 &&
               !strcmp(output, outputs[chosen]) && !strcmp(state, "connected");
      continue;
    }
    if (!active || !(token = strtok_r(line, " \t\r\n", &save))) continue;
    {
      unsigned int w, h; char extra;
      if (sscanf(token, "%ux%u%c", &w, &h, &extra) != 2 || !w || !h) continue;
    }
    snprintf(mode, sizeof mode, "%s", token);
    while ((token = strtok_r(NULL, " \t\r\n", &save))) {
      char **next;
      double value = strtod(token, &end);
      if (end == token || value <= 0 || strspn(end, "*+") != strlen(end)) continue;
      *end = '\0'; snprintf(rate, sizeof rate, "%s", token);
      next = realloc(modes, (nmode + 1) * sizeof *modes); if (!next) die("realloc:"); modes = next;
      next = realloc(rates, (nmode + 1) * sizeof *rates); if (!next) die("realloc:"); rates = next;
      next = realloc(labels, (nmode + 1) * sizeof *labels); if (!next) die("realloc:"); labels = next;
      modes[nmode] = strdup(mode); rates[nmode] = strdup(rate);
      snprintf(title, sizeof title, "%s   /   %s Hz", mode, rate);
      labels[nmode] = strdup(title);
      if (!modes[nmode] || !rates[nmode] || !labels[nmode]) die("strdup:");
      nmode++;
    }
  }
  pclose(fp);
  snprintf(title, sizeof title, "R E F R E S H   R A T E   /   %s", outputs[chosen]);
  if (!nmode) {
    char *notice[] = {"No display modes reported by xrandr."};
    displaypick(title, notice, 1);
    goto cleanup;
  }
  selectedmode = displaypick(title, labels, nmode);
  if (selectedmode >= 0 && fork() == 0) {
    /* Do not block dwm while xrandr changes the screen. Persist only success. */
    close(ConnectionNumber(dpy));
    signal(SIGCHLD, SIG_DFL);
    if (!displayapply(outputs[chosen], modes[selectedmode], rates[selectedmode])) {
      fprintf(stderr, "dwm: could not apply display mode for %s\n", outputs[chosen]);
      _exit(1);
    }
    if (!displaysave(outputs[chosen], modes[selectedmode], rates[selectedmode])) {
      fprintf(stderr, "dwm: could not save display mode: %s\n", strerror(errno));
      _exit(1);
    }
    _exit(0);
  }
cleanup:
  for (i = 0; i < nout; i++) free(outputs[i]);
  for (i = 0; i < nmode; i++) { free(modes[i]); free(rates[i]); free(labels[i]); }
  free(outputs); free(modes); free(rates); free(labels);
}

/* The picker is native dwm/Xlib like Alt-Tab. Resolve Imlib2 at runtime so
 * dwm still builds and the filenames remain usable if previews are absent. */
typedef struct {
  void *library;
  void (*display)(Display *);
  void (*visual)(Visual *);
  void (*colormap)(Colormap);
  void (*drawable)(Drawable);
  void *(*load)(const char *);
  void (*image)(void *);
  int (*width)(void);
  int (*height)(void);
  void (*render)(int, int, int, int);
  void (*freeimage)(void);
  void *(*scaled)(int, int, int, int, int, int);
  void (*save)(const char *);
} WallpaperImages;

static int
wallpaperimagesinit(WallpaperImages *api)
{
  memset(api, 0, sizeof *api);
  api->library = dlopen("libImlib2.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!api->library) return 0;
#define LOADIMLIB(member, name) do { \
  *(void **)(&api->member) = dlsym(api->library, name); \
  if (!api->member) { dlclose(api->library); memset(api, 0, sizeof *api); return 0; } \
} while (0)
  LOADIMLIB(display, "imlib_context_set_display");
  LOADIMLIB(visual, "imlib_context_set_visual");
  LOADIMLIB(colormap, "imlib_context_set_colormap");
  LOADIMLIB(drawable, "imlib_context_set_drawable");
  LOADIMLIB(load, "imlib_load_image");
  LOADIMLIB(image, "imlib_context_set_image");
  LOADIMLIB(width, "imlib_image_get_width");
  LOADIMLIB(height, "imlib_image_get_height");
  LOADIMLIB(render, "imlib_render_image_on_drawable_at_size");
  LOADIMLIB(freeimage, "imlib_free_image");
  LOADIMLIB(scaled, "imlib_create_cropped_scaled_image");
  LOADIMLIB(save, "imlib_save_image");
#undef LOADIMLIB
  api->display(dpy);
  api->visual(DefaultVisual(dpy, screen));
  api->colormap(DefaultColormap(dpy, screen));
  return 1;
}

/* Decode only the currently selected image in a disposable helper. dwm never
 * waits for a large wallpaper to decode; navigating cancels the old helper. */
typedef struct {
  pid_t pid;
  int fd;
  char path[64];
} WallpaperPreview;

static void
wallpaperpreviewcancel(WallpaperPreview *p)
{
  if (p->pid > 0) kill(p->pid, SIGTERM);
  if (p->fd >= 0) close(p->fd);
  if (p->path[0]) unlink(p->path);
  p->pid = 0; p->fd = -1; p->path[0] = '\0';
}

static int
wallpaperpreviewstart(WallpaperPreview *p, WallpaperImages *api, const char *source, int maxw, int maxh)
{
  int fd, fds[2];
  pid_t child;
  wallpaperpreviewcancel(p);
  if (maxw < 1 || maxh < 1) return 0;
  snprintf(p->path, sizeof p->path, "/tmp/dwm-wallpaper-XXXXXX.png");
  fd = mkstemps(p->path, 4);
  if (fd < 0) { p->path[0] = '\0'; return 0; }
  close(fd);
  if (pipe(fds) < 0) { unlink(p->path); p->path[0] = '\0'; return 0; }
  child = fork();
  if (child == 0) {
    void *image, *small;
    int w, h, swidth, sheight, ok = 0;
    struct stat st;
    close(fds[0]);
    close(ConnectionNumber(dpy));
    image = api->load(source);
    if (image) {
      api->image(image); w = api->width(); h = api->height();
      if (w > 0 && h > 0) {
        swidth = maxw; sheight = (int)((double)h * swidth / w);
        if (sheight > maxh) { sheight = maxh; swidth = (int)((double)w * sheight / h); }
        small = api->scaled(0, 0, w, h, MAX(1, swidth), MAX(1, sheight));
        if (small) {
          api->image(small); api->save(p->path); api->freeimage();
          if (!stat(p->path, &st) && st.st_size > 0) ok = 1;
        }
      }
      api->image(image); api->freeimage();
    }
    (void)write(fds[1], &ok, sizeof ok);
    close(fds[1]);
    _exit(0);
  }
  close(fds[1]);
  if (child < 0) { close(fds[0]); unlink(p->path); p->path[0] = '\0'; return 0; }
  p->pid = child; p->fd = fds[0];
  return 1;
}

static int
wallpapername(const char *name)
{
  const char *ext = strrchr(name, '.');
  return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") ||
                 !strcasecmp(ext, ".png") || !strcasecmp(ext, ".webp") ||
                 !strcasecmp(ext, ".bmp") || !strcasecmp(ext, ".gif"));
}

static int
wallpapercompare(const void *a, const void *b)
{
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
wallpaperfree(char **paths, size_t count)
{
  size_t i;
  for (i = 0; i < count; i++) free(paths[i]);
  free(paths);
}

static char **
wallpaperscan(const char *dir, size_t *count, char *message, size_t message_size)
{
  DIR *dp;
  struct dirent *entry;
  struct stat st;
  char **paths = NULL, *path;
  size_t capacity = 0, len;
  *count = 0;
  if (!(dp = opendir(dir))) {
    snprintf(message, message_size, "Cannot open folder: %s", strerror(errno));
    return NULL;
  }
  while ((entry = readdir(dp))) {
    if (entry->d_name[0] == '.' || !wallpapername(entry->d_name)) continue;
    len = strlen(dir) + strlen(entry->d_name) + 2;
    path = ecalloc(len, 1);
    snprintf(path, len, "%s/%s", dir, entry->d_name);
    if (stat(path, &st) || !S_ISREG(st.st_mode)) { free(path); continue; }
    if (*count == capacity) {
      char **more;
      capacity = capacity ? capacity * 2 : 32;
      more = realloc(paths, capacity * sizeof *paths);
      if (!more) { free(path); wallpaperfree(paths, *count); closedir(dp); die("realloc:"); }
      paths = more;
    }
    paths[(*count)++] = path;
  }
  closedir(dp);
  if (*count) {
    qsort(paths, *count, sizeof *paths, wallpapercompare);
    snprintf(message, message_size, "%zu images  /  Enter apply  /  R refresh  /  Esc close", *count);
  } else snprintf(message, message_size, "No images found. Add files, then press R to refresh.");
  return paths;
}

/* Run feh off the event thread. A helper reports its exit status through a
 * pipe because dwm normally auto-reaps children. */
static int
wallpaperapplystart(const char *path)
{
  int fds[2];
  pid_t helper;
  if (pipe(fds) < 0) return -1;
  helper = fork();
  if (helper == 0) {
    int status, result = 1;
    pid_t child;
    close(fds[0]);
    close(ConnectionNumber(dpy));
    signal(SIGCHLD, SIG_DFL);
    child = fork();
    if (child == 0) {
      close(fds[1]);
      execlp("feh", "feh", "--bg-fill", "--", path, (char *)NULL);
      _exit(127);
    }
    if (child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status)) result = WEXITSTATUS(status);
    (void)write(fds[1], &result, sizeof result);
    close(fds[1]);
    _exit(0);
  }
  close(fds[1]);
  if (helper < 0) { close(fds[0]); return -1; }
  return fds[0];
}

void wallpaperswitcher(const Arg *arg)
{
  Monitor *m = selmon;
  XSetWindowAttributes wa;
  XEvent ev;
  Window popup;
  Drw *d;
  Clr *head, *normal, *selected, *blue;
  const char *font[] = {"monospace:size=10", "Symbols Nerd Font:size=11"};
  const char *hc[] = {"#8a949e", "#090b0d", "#2d3035"};
  const char *nc[] = {"#adbac7", "#121417", "#1e2025"};
  const char *sc[] = {"#f2d49e", "#181a1e", "#f2d49e"};
  const char *bc[] = {"#7fa8c9", "#090b0d", "#1e2025"};
  const char *folder = getenv("DWM_WALLPAPERS_DIR"), *home = getenv("HOME");
  char *directory, **paths, *base, message[512], line[512];
  size_t count = 0, choice = 0, start, row;
  int width, height, rh, header, listw, previewx, previeww, previewh, visible, done = 0;
  int imlib, imagewidth, imageheight, pw, ph, preview_failed = 0, applyfd = -1;
  WallpaperPreview worker = {0, -1, ""};
  void *image = NULL;
  size_t loaded = (size_t)-1;
  WallpaperImages api;
  KeySym key;
  (void)arg;
  if (folder && *folder) directory = strdup(folder);
  else if (home && *home) {
    directory = ecalloc(strlen(home) + sizeof "/Downloads/wallpaper", 1);
    sprintf(directory, "%s/Downloads/wallpaper", home);
  } else directory = strdup(".");
  if (!directory) die("strdup:");
  paths = wallpaperscan(directory, &count, message, sizeof message);
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
    wallpaperfree(paths, count); free(directory); return;
  }
  imlib = wallpaperimagesinit(&api);
  width = MAX(1, MIN(1000, m->mw - 20));
  height = MAX(1, MIN(600, m->mh - 20));
  rh = MAX(28, bh + 5); header = 3 * rh;
  listw = width >= 550 ? width * 38 / 100 : width - 32;
  previewx = 16 + listw + 16;
  previeww = width - previewx - 16;
  previewh = height - header - 2 * rh - 16;
  visible = MAX(1, (height - header - 2 * rh) / rh);
  wa.override_redirect = True; wa.background_pixel = BlackPixel(dpy, screen);
  wa.event_mask = ExposureMask | ButtonPressMask;
  popup = XCreateWindow(dpy, root, m->mx + (m->mw - width) / 2,
                        m->my + (m->mh - height) / 2, width, height, 1,
                        DefaultDepth(dpy, screen), CopyFromParent, DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  d = drw_create(dpy, screen, root, width, height);
  drw_fontset_create(d, font, LENGTH(font));
  head = drw_scm_create(d, hc, 3); normal = drw_scm_create(d, nc, 3);
  selected = drw_scm_create(d, sc, 3); blue = drw_scm_create(d, bc, 3);
  XSetWindowBorder(dpy, popup, head[ColBorder].pixel); XMapRaised(dpy, popup);
  while (!done && running) {
    if (count && loaded != choice) {
      wallpaperpreviewcancel(&worker);
      if (image) { api.image(image); api.freeimage(); image = NULL; }
      preview_failed = !imlib || !wallpaperpreviewstart(&worker, &api, paths[choice], previeww - 16, previewh - 16);
      loaded = choice;
    }
    start = count && choice >= (size_t)visible ? choice - visible + 1 : 0;
    drw_setscheme(d, head); drw_rect(d, 0, 0, width, height, 1, 1);
    drw_text(d, 16, 12, MAX(1, width - 32), rh, 4, "W A L L P A P E R S", 0);
    drw_setscheme(d, blue);
    if (width >= 550) drw_text(d, previewx, 12, MAX(1, previeww), rh, 4, "P R E V I E W", 0);
    drw_setscheme(d, head);
    drw_rect(d, 16, 12 + rh, MAX(1, width - 32), 1, 1, 0);
    drw_text(d, 16, 16 + rh, MAX(1, width - 32), rh, 4, directory, 0);
    for (row = 0; row < (size_t)visible; row++) {
      size_t i = start + row;
      drw_setscheme(d, i < count && i == choice ? selected : normal);
      base = i < count ? strrchr(paths[i], '/') : NULL;
      snprintf(line, sizeof line, "%s", i < count ? (base ? base + 1 : paths[i]) : (count ? "" : "No wallpapers"));
      drw_text(d, 16, header + row * rh, MAX(1, listw), rh - 2, 10, line, 0);
    }
    if (previeww > 0 && previewh > rh) {
      drw_setscheme(d, normal);
      drw_rect(d, previewx, header, previeww, previewh, 1, 1);
      if (count && image) {
        api.image(image); imagewidth = api.width(); imageheight = api.height();
        if (imagewidth > 0 && imageheight > 0) {
          pw = MAX(1, previeww - 16); ph = (int)((double)imageheight * pw / imagewidth);
          if (ph > previewh - 16) { ph = previewh - 16; pw = (int)((double)imagewidth * ph / imageheight); }
          pw = MAX(1, pw); ph = MAX(1, ph);
          api.drawable(d->drawable);
          api.render(previewx + (previeww - pw) / 2, header + (previewh - ph) / 2, pw, ph);
        }
      } else {
        drw_setscheme(d, normal);
        drw_text(d, previewx, header + rh, previeww, rh, 12,
                 !imlib ? "Install Imlib2 for image previews" : !count ? "Select a wallpaper" : preview_failed ? "Preview unavailable for this image" : "Loading preview...", 0);
      }
      drw_setscheme(d, selected);
      drw_text(d, previewx, height - 2 * rh - 8, previeww, rh, 12, "SET WALLPAPER  /  ENTER", 0);
    }
    drw_setscheme(d, head);
    drw_text(d, 16, height - rh - 4, MAX(1, width - 32), rh, 4, message, 0);
    drw_map(d, popup, 0, 0, width, height);
    /* Wait for X input, preview completion or feh without freezing dwm. */
    if (!XPending(dpy) && (worker.fd >= 0 || applyfd >= 0)) {
      fd_set fds;
      int xfd = ConnectionNumber(dpy), ready, maxfd = xfd;
      FD_ZERO(&fds); FD_SET(xfd, &fds);
      if (worker.fd >= 0) { FD_SET(worker.fd, &fds); maxfd = MAX(maxfd, worker.fd); }
      if (applyfd >= 0) { FD_SET(applyfd, &fds); maxfd = MAX(maxfd, applyfd); }
      ready = select(maxfd + 1, &fds, NULL, NULL, NULL);
      if (ready < 0) { if (errno == EINTR) continue; done = 1; continue; }
      if (applyfd >= 0 && FD_ISSET(applyfd, &fds)) {
        int result = 1;
        if (read(applyfd, &result, sizeof result) != sizeof result) result = 1;
        close(applyfd); applyfd = -1;
        if (result == 0) done = 1;
        else snprintf(message, sizeof message, "Could not apply. Check feh and the selected image.");
        continue;
      }
      if (worker.fd >= 0 && FD_ISSET(worker.fd, &fds)) {
        int ok = 0;
        if (read(worker.fd, &ok, sizeof ok) != sizeof ok) ok = 0;
        close(worker.fd); worker.fd = -1; worker.pid = 0;
        if (ok) image = api.load(worker.path);
        preview_failed = !image;
        unlink(worker.path); worker.path[0] = '\0';
        continue;
      }
    }
    XNextEvent(dpy, &ev);
    if (ev.type == KeyPress) {
      key = XLookupKeysym(&ev.xkey, 0);
      if (key == XK_Escape) done = 1;
      else if (key == XK_r || key == XK_R) {
        wallpaperpreviewcancel(&worker);
        if (image) { api.image(image); api.freeimage(); image = NULL; }
        wallpaperfree(paths, count);
        paths = wallpaperscan(directory, &count, message, sizeof message);
        choice = 0; loaded = (size_t)-1;
      } else if (count && (key == XK_Down || key == XK_j)) choice = (choice + 1) % count;
      else if (count && (key == XK_Up || key == XK_k)) choice = (choice + count - 1) % count;
      else if (count && key == XK_Home) choice = 0;
      else if (count && key == XK_End) choice = count - 1;
      else if (count && (key == XK_Return || key == XK_KP_Enter)) {
        if (applyfd < 0) {
          applyfd = wallpaperapplystart(paths[choice]);
          snprintf(message, sizeof message, "%s", applyfd < 0 ? "Could not start feh." : "Applying wallpaper...");
        }
      }
    } else if (ev.type == ButtonPress && ev.xbutton.window == popup) {
      int x = ev.xbutton.x, y = ev.xbutton.y;
      if (count && ev.xbutton.button == Button4) choice = (choice + count - 1) % count;
      else if (count && ev.xbutton.button == Button5) choice = (choice + 1) % count;
      else if (ev.xbutton.button == Button1 && x >= 16 && x < 16 + listw && y >= header && y < header + visible * rh) {
        size_t i = start + (y - header) / rh;
        if (i < count) choice = i;
      } else if (count && ev.xbutton.button == Button1 && previeww > 0 && x >= previewx && x < previewx + previeww && y >= height - 2 * rh - 8 && y < height - rh - 8) {
        if (applyfd < 0) {
          applyfd = wallpaperapplystart(paths[choice]);
          snprintf(message, sizeof message, "%s", applyfd < 0 ? "Could not start feh." : "Applying wallpaper...");
        }
      }
    } else if (ev.type == ConfigureNotify && ev.xconfigure.window == root) {
      done = 1; configurenotify(&ev);
    } else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify &&
               ev.type != FocusIn && ev.type != KeyRelease && handler[ev.type]) handler[ev.type](&ev);
  }
  wallpaperpreviewcancel(&worker);
  if (applyfd >= 0) close(applyfd);
  if (image) { api.image(image); api.freeimage(); }
  if (imlib) dlclose(api.library);
  XDestroyWindow(dpy, popup); XUngrabKeyboard(dpy, CurrentTime);
  drw_scm_free(d, head, 3); drw_scm_free(d, normal, 3);
  drw_scm_free(d, selected, 3); drw_scm_free(d, blue, 3); drw_free(d);
  wallpaperfree(paths, count); free(directory);
}

/* The list keeps window IDs rather than Client pointers because windows can
 * close while the keyboard is grabbed.  Composite redirects are temporary and
 * automatic, so an existing compositor can continue managing the screen. */
void alttab(const Arg *arg) {
  Monitor *m = selmon;
  Client *c;
  Window *wins, *list, popup;
  Damage *damage;
  XSetWindowAttributes wa;
  XEvent ev;
  Drw *d;
  Clr *normal, *selected, *head, *muted, *blue;
  const char *font[] = {"monospace:size=10", "Symbols Nerd Font:size=11"};
  const char *nc[] = {"#adbac7", "#121417", "#1e2025"};
  const char *sc[] = {"#f2d49e", "#181a1e", "#f2d49e"};
  const char *hc[] = {"#8a949e", "#090b0d", "#2d3035"};
  const char *mc[] = {"#8a949e", "#121417", "#1e2025"};
  const char *bc[] = {"#7fa8c9", "#090b0d", "#1e2025"};
  unsigned int n = 0, count = 0, i, row, start, visible, filter = 0;
  int choice, done = 0, accept = 0, width, height, rh, header, listw, pad;
  int composite_event, composite_error, damage_event, damage_error, render_event, render_error;
  int havepreview, major, minor, (*oldhandler)(Display *, XErrorEvent *);
  char text[512], tagtext[128];
  KeySym key;

  for (c = m->stack; c; c = c->snext) n++;
  if (!n) return;
  wins = ecalloc(n, sizeof(Window)); list = ecalloc(n, sizeof(Window));
  damage = ecalloc(n, sizeof(Damage));
  for (c = m->stack, i = 0; c; c = c->snext) wins[i++] = c->win;
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
    free(wins); free(list); free(damage); return;
  }
  havepreview = XCompositeQueryExtension(dpy, &composite_event, &composite_error) &&
                XDamageQueryExtension(dpy, &damage_event, &damage_error) &&
                XRenderQueryExtension(dpy, &render_event, &render_error) &&
                XCompositeQueryVersion(dpy, &major, &minor) &&
                (major > 0 || minor >= 2);
  if (havepreview) {
    XSync(dpy, False); oldhandler = XSetErrorHandler(xerrordummy);
    for (i = 0; i < n; i++) {
      XCompositeRedirectWindow(dpy, wins[i], CompositeRedirectAutomatic);
      damage[i] = XDamageCreate(dpy, wins[i], XDamageReportNonEmpty);
    }
    XSync(dpy, False); XSetErrorHandler(oldhandler);
  }
  pad = 16; rh = MAX(32, bh + 8); header = 3 * rh;
  width = MAX(1, MIN(960, m->mw - 20));
  height = MAX(1, MIN(530, m->mh - 20));
  /* On narrow screens use the whole card for the list and omit the preview. */
  listw = width >= 500 ? width * 38 / 100 : width - 2 * pad;
  visible = MAX(1, (height - header - 2 * pad - rh) / rh);
  wa.override_redirect = True; wa.background_pixel = BlackPixel(dpy, screen);
  wa.event_mask = ExposureMask;
  popup = XCreateWindow(dpy, root, m->mx + (m->mw - width) / 2, m->my + (m->mh - height) / 2,
                        width, height, 1, DefaultDepth(dpy, screen), CopyFromParent,
                        DefaultVisual(dpy, screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  d = drw_create(dpy, screen, root, width, height);
  drw_fontset_create(d, font, LENGTH(font));
  normal = drw_scm_create(d, nc, 3); selected = drw_scm_create(d, sc, 3);
  head = drw_scm_create(d, hc, 3); muted = drw_scm_create(d, mc, 3); blue = drw_scm_create(d, bc, 3);
  XSetWindowBorder(dpy, popup, head[ColBorder].pixel); XMapRaised(dpy, popup);
  choice = n > 1 ? (arg->i < 0 ? n - 1 : 1) : 0;
  while (!done && running) {
    count = 0;
    for (i = 0; i < n; i++) {
      c = wintoclient(wins[i]);
      if (c && c->mon == m && (!filter || (c->tags & filter))) list[count++] = wins[i];
    }
    if (choice >= (int)count) choice = count ? count - 1 : 0;
    start = count && choice >= (int)visible ? choice - visible + 1 : 0;
    drw_setscheme(d, head); drw_rect(d, 0, 0, width, height, 1, 1);
    drw_text(d, pad, pad, width - 2 * pad, rh, 4, "W I N D O W S", 0);
    drw_setscheme(d, blue);
    drw_text(d, width - pad - 120, pad, 120, rh, 4, "LIVE PREVIEW", 0);
    drw_setscheme(d, head);
    drw_rect(d, pad, pad + rh, MAX(1, width - 2 * pad), 1, 1, 0);
    tagtext[0] = '\0';
    for (i = 0; i < LENGTH(tags); i++) {
      char part[16];
      snprintf(part, sizeof part, "%s%u%s ", filter == (1U << i) ? "[" : "", i + 1, filter == (1U << i) ? "]" : "");
      strncat(tagtext, part, sizeof(tagtext) - strlen(tagtext) - 1);
    }
    snprintf(text, sizeof text, "TAGS   %s   0 ALL", tagtext);
    drw_text(d, pad, pad + rh + 4, width - 2 * pad, rh, 4, text, 0);
    for (row = 0; row < visible; row++) {
      i = start + row;
      drw_setscheme(d, i < count && (int)i == choice ? selected : normal);
      if (i < count && (c = wintoclient(list[i]))) {
        unsigned int t;
        tagtext[0] = '\0';
        for (t = 0; t < LENGTH(tags); t++) if (c->tags & (1U << t)) {
          char part[8]; snprintf(part, sizeof part, "%u ", t + 1);
          strncat(tagtext, part, sizeof(tagtext) - strlen(tagtext) - 1);
        }
        snprintf(text, sizeof text, "%s | %s", tagtext, c->name);
      } else snprintf(text, sizeof text, "%s", count ? "" : "No windows on this tag");
      drw_text(d, pad, header + row * rh, listw, rh - 2, 10, text, 0);
    }
    if (width >= 500) {
      int px = pad + listw + pad, py = header, pw = width - px - pad, ph = height - py - pad - rh;
      if (pw > 0 && ph > rh) {
        drw_setscheme(d, normal); drw_rect(d, px, py, pw, ph, 1, 1);
        drw_setscheme(d, muted);
        if (count && (c = wintoclient(list[choice]))) {
          drw_text(d, px, py, pw, rh, 10, c->name, 0);
          if (!havepreview || !switcherpreview(d, c->win, px + 8, py + rh, pw - 16, ph - rh - 8))
            drw_text(d, px, py + rh, pw, rh, 10, "Preview unavailable", 0);
        } else drw_text(d, px, py, pw, rh, 10, "Select a window to preview", 0);
      }
    }
    drw_setscheme(d, head);
    drw_text(d, pad, height - pad - rh, width - 2 * pad, rh, 4,
             "TAB cycle   DEL close   1-9 tag   ESC cancel   release ALT select", 0);
    drw_map(d, popup, 0, 0, width, height);
    XNextEvent(dpy, &ev);
    if (havepreview && ev.type == damage_event + XDamageNotify) {
      XDamageNotifyEvent *de = (XDamageNotifyEvent *)&ev;
      XSync(dpy, False); oldhandler = XSetErrorHandler(xerrordummy);
      XDamageSubtract(dpy, de->damage, None, None);
      /* Collapse bursts of updates so fast Alt-Tab does not lag behind video. */
      while (XCheckTypedEvent(dpy, damage_event + XDamageNotify, &ev)) {
        de = (XDamageNotifyEvent *)&ev;
        XDamageSubtract(dpy, de->damage, None, None);
      }
      XSync(dpy, False); XSetErrorHandler(oldhandler);
    } else if (ev.type == KeyPress) {
      key = XLookupKeysym(&ev.xkey, 0);
      if (key == XK_Escape) done = 1;
      else if ((key == XK_Delete || key == XK_KP_Delete) && count) {
        /* Request a normal close without changing focus or the viewed tag. */
        c = wintoclient(list[choice]);
        if (c) closeclient(c);
      }
      else if (key == XK_Return || key == XK_KP_Enter) done = accept = 1;
      else if ((key == XK_Tab || key == XK_ISO_Left_Tab || key == XK_Down || key == XK_Up) && count) {
        int reverse = key == XK_Up || key == XK_ISO_Left_Tab || (ev.xkey.state & ShiftMask);
        choice = (choice + (reverse ? count - 1 : 1)) % count;
      } else if (key >= XK_0 && key <= XK_9) {
        filter = key == XK_0 ? 0 : 1U << (key - XK_1); choice = 0;
      }
    } else if (ev.type == KeyRelease) {
      key = XLookupKeysym(&ev.xkey, 0);
      if (key == XK_Alt_L || key == XK_Alt_R) done = accept = 1;
    } else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify && ev.type != FocusIn && handler[ev.type]) handler[ev.type](&ev);
  }
  c = accept && count ? wintoclient(list[choice]) : NULL;
  XDestroyWindow(dpy, popup); XUngrabKeyboard(dpy, CurrentTime);
  if (havepreview) {
    XSync(dpy, False); oldhandler = XSetErrorHandler(xerrordummy);
    for (i = 0; i < n; i++) {
      if (damage[i]) XDamageDestroy(dpy, damage[i]);
      XCompositeUnredirectWindow(dpy, wins[i], CompositeRedirectAutomatic);
    }
    XSync(dpy, False); XSetErrorHandler(oldhandler);
    /* Extension events must not reach dwm's core-event dispatch table. */
    while (XCheckTypedEvent(dpy, damage_event + XDamageNotify, &ev)) { }
  }
  drw_scm_free(d, normal, 3); drw_scm_free(d, selected, 3); drw_scm_free(d, head, 3);
  drw_scm_free(d, muted, 3); drw_scm_free(d, blue, 3); drw_free(d);
  free(wins); free(list); free(damage);
  if (c && c->mon == selmon) {
    if (!ISVISIBLE(c)) { Arg a = {.ui = c->tags}; view(&a); }
    focus(c); restack(selmon);
  }
}

/* Preview each monitor's current tag as a miniature layout. Window IDs, not
 * Client pointers, are retained across events so closing a window is safe. */
void monitorswitcher(const Arg *arg) {
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
  const char *sc[] = {"#f2d49e", "#181a1e", "#f2d49e"};
  const char *bc[] = {"#7fa8c9", "#090b0d", "#1e2025"};
  unsigned int nm = 0, nw = 0, i, j, first, slots, active = 0;
  int width, height, pad = 16, rh, cardw, cardh, previewh;
  int done = 0, accept = 0, havepreview, ce, cr, de, dr, re, rr, major, minor;
  int (*oldhandler)(Display *, XErrorEvent *);
  char text[512];
  KeySym key;

  (void)arg;
  for (m = mons; m; m = m->next) {
    nm++;
    for (c = m->clients; c; c = c->next) if (ISVISIBLE(c)) nw++;
  }
  if (nm < 2) return;
  screens = ecalloc(nm, sizeof(Monitor *));
  wins = ecalloc(MAX(1, nw), sizeof(Window));
  damage = ecalloc(MAX(1, nw), sizeof(Damage));
  for (m = mons, i = 0, j = 0; m; m = m->next, i++) {
    screens[i] = m;
    if (m == origin) active = i;
    for (c = m->clients; c; c = c->next) if (ISVISIBLE(c)) wins[j++] = c->win;
  }
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
    free(screens); free(wins); free(damage); return;
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
  height = MAX(1, MIN(500, origin->mh - 20));
  rh = MAX(28, bh + 4);
  slots = MIN(nm, width >= 850 ? 3U : (width >= 450 ? 2U : 1U));
  cardw = MAX(1, (width - (int)(slots + 1) * pad) / (int)slots);
  cardh = MAX(1, height - 3 * rh - 2 * pad);
  previewh = MAX(1, cardh * 2 / 3 - rh);
  wa.override_redirect = True; wa.background_pixel = BlackPixel(dpy, screen);
  wa.event_mask = ExposureMask;
  popup = XCreateWindow(dpy, root, origin->mx + (origin->mw - width) / 2,
                        origin->my + (origin->mh - height) / 2, width, height, 1,
                        DefaultDepth(dpy, screen), CopyFromParent, DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  d = drw_create(dpy, screen, root, width, height);
  drw_fontset_create(d, font, LENGTH(font));
  head = drw_scm_create(d, hc, 3); normal = drw_scm_create(d, nc, 3);
  selected = drw_scm_create(d, sc, 3); blue = drw_scm_create(d, bc, 3);
  XSetWindowBorder(dpy, popup, head[ColBorder].pixel); XMapRaised(dpy, popup);
  active = (active + 1) % nm;
  while (!done && running) {
    first = (active / slots) * slots;
    drw_setscheme(d, head); drw_rect(d, 0, 0, width, height, 1, 1);
    drw_text(d, pad, pad, MAX(1, width - 2 * pad), rh, 4, "M O N I T O R S", 0);
    drw_setscheme(d, blue);
    if (width > 300) drw_text(d, width - pad - 120, pad, 120, rh, 4, "LIVE PREVIEW", 0);
    drw_setscheme(d, head);
    drw_rect(d, pad, pad + rh, MAX(1, width - 2 * pad), 1, 1, 0);
    for (i = 0; i < slots && first + i < nm; i++) {
      int x = pad + i * (cardw + pad), y = pad + rh + 12;
      int px = x + 8, py = y + rh + 4, pw = MAX(1, cardw - 16);
      int listy = py + previewh + 8, lines = 0;
      m = screens[first + i];
      drw_setscheme(d, first + i == active ? selected : normal);
      drw_rect(d, x, y, cardw, cardh, 1, 1);
      drw_rect(d, x, y, cardw, cardh, 0, 0);
      snprintf(text, sizeof text, "DISPLAY %u%s", m->num + 1, m == origin ? "  / CURRENT" : "");
      drw_text(d, x + 1, y + 1, MAX(1, cardw - 2), rh, 8, text, 0);
      drw_setscheme(d, head); drw_rect(d, px, py, pw, previewh, 1, 1);
      /* Use dwm's window geometries to preserve the spatial layout. */
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
      drw_setscheme(d, normal);
      for (j = 0; j < nw && listy + (lines + 1) * rh < y + cardh; j++) {
        c = wintoclient(wins[j]);
        if (!c || c->mon != m || !ISVISIBLE(c)) continue;
        snprintf(text, sizeof text, "%s %s", c == m->sel ? ">" : "-", c->name);
        drw_text(d, x + 1, listy + lines++ * rh, MAX(1, cardw - 2), rh, 8, text, 0);
      }
      if (!lines && listy + rh < y + cardh)
        drw_text(d, x + 1, listy, MAX(1, cardw - 2), rh, 8, "No windows on this tag", 0);
    }
    drw_setscheme(d, head);
    drw_text(d, pad, height - pad - rh, MAX(1, width - 2 * pad), rh, 4,
             "ALT+` / ARROWS switch   ESC cancel   release ALT select", 0);
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
      else if (key == XK_grave || key == XK_Tab || key == XK_Right || key == XK_Left) {
        int reverse = key == XK_Left || (ev.xkey.state & ShiftMask);
        active = (active + (reverse ? nm - 1 : 1)) % nm;
      }
    } else if (ev.type == KeyRelease) {
      key = XLookupKeysym(&ev.xkey, 0);
      if (key == XK_Alt_L || key == XK_Alt_R) done = accept = 1;
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
    while (XCheckTypedEvent(dpy, de + XDamageNotify, &ev)) { }
  }
  drw_scm_free(d, head, 3); drw_scm_free(d, normal, 3);
  drw_scm_free(d, selected, 3); drw_scm_free(d, blue, 3); drw_free(d);
  if (accept && screens[active] != selmon) {
    unfocus(selmon->sel, 0); selmon = screens[active]; focus(NULL); warp(selmon->sel);
  }
  free(screens); free(wins); free(damage);
}

void applyrules(Client *c) {
  const char *class, *instance;
  unsigned int i;
  const Rule *r;
  Monitor *m;
  XClassHint ch = {NULL, NULL};

  /* rule matching */
  c->isfloating = 0;
  c->tags = 0;
  XGetClassHint(dpy, c->win, &ch);
  class = ch.res_class ? ch.res_class : broken;
  instance = ch.res_name ? ch.res_name : broken;

  for (i = 0; i < LENGTH(rules); i++) {
    r = &rules[i];
    if ((!r->title || strstr(c->name, r->title)) &&
        (!r->class || strstr(class, r->class)) &&
        (!r->instance || strstr(instance, r->instance))) {
      c->isfloating = r->isfloating;
      c->tags |= r->tags;
      for (m = mons; m && m->num != r->monitor; m = m->next)
        ;
      if (m)
        c->mon = m;
    }
  }
  if (ch.res_class)
    XFree(ch.res_class);
  if (ch.res_name)
    XFree(ch.res_name);
  c->tags =
      c->tags & TAGMASK ? c->tags & TAGMASK : c->mon->tagset[c->mon->seltags];
}

int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact) {
  int baseismin;
  Monitor *m = c->mon;

  /* set minimum possible */
  *w = MAX(1, *w);
  *h = MAX(1, *h);
  if (interact) {
    if (*x > sw)
      *x = sw - WIDTH(c);
    if (*y > sh)
      *y = sh - HEIGHT(c);
    if (*x + *w + 2 * c->bw < 0)
      *x = 0;
    if (*y + *h + 2 * c->bw < 0)
      *y = 0;
  } else {
    if (*x >= m->wx + m->ww)
      *x = m->wx + m->ww - WIDTH(c);
    if (*y >= m->wy + m->wh)
      *y = m->wy + m->wh - HEIGHT(c);
    if (*x + *w + 2 * c->bw <= m->wx)
      *x = m->wx;
    if (*y + *h + 2 * c->bw <= m->wy)
      *y = m->wy;
  }
  if (*h < bh)
    *h = bh;
  if (*w < bh)
    *w = bh;
  if (resizehints || c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
    if (!c->hintsvalid)
      updatesizehints(c);
    /* see last two sentences in ICCCM 4.1.2.3 */
    baseismin = c->basew == c->minw && c->baseh == c->minh;
    if (!baseismin) { /* temporarily remove base dimensions */
      *w -= c->basew;
      *h -= c->baseh;
    }
    /* adjust for aspect limits */
    if (c->mina > 0 && c->maxa > 0) {
      if (c->maxa < (float)*w / *h)
        *w = *h * c->maxa + 0.5;
      else if (c->mina < (float)*h / *w)
        *h = *w * c->mina + 0.5;
    }
    if (baseismin) { /* increment calculation requires this */
      *w -= c->basew;
      *h -= c->baseh;
    }
    /* adjust for increment value */
    if (c->incw)
      *w -= *w % c->incw;
    if (c->inch)
      *h -= *h % c->inch;
    /* restore base dimensions */
    *w = MAX(*w + c->basew, c->minw);
    *h = MAX(*h + c->baseh, c->minh);
    if (c->maxw)
      *w = MIN(*w, c->maxw);
    if (c->maxh)
      *h = MIN(*h, c->maxh);
  }
  return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void arrange(Monitor *m) {
  if (m)
    showhide(m->stack);
  else
    for (m = mons; m; m = m->next)
      showhide(m->stack);
  if (m) {
    arrangemon(m);
    restack(m);
  } else
    for (m = mons; m; m = m->next)
      arrangemon(m);
}

void arrangemon(Monitor *m) {
  strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, sizeof m->ltsymbol);
  if (m->lt[m->sellt]->arrange)
    m->lt[m->sellt]->arrange(m);
}

void attach(Client *c) {
  c->next = c->mon->clients;
  c->mon->clients = c;
}

void attachstack(Client *c) {
  c->snext = c->mon->stack;
  c->mon->stack = c;
}

void buttonpress(XEvent *e) {
  unsigned int i, x, click, occ = 0;
  Arg arg = {0};
  Client *c;
  Monitor *m;
  XButtonPressedEvent *ev = &e->xbutton;

  click = ClkRootWin;
  /* focus monitor if necessary */
  if ((m = wintomon(ev->window)) && m != selmon) {
    unfocus(selmon->sel, 1);
    selmon = m;
    focus(NULL);
  }
  if (ev->window == selmon->barwin) {
#ifdef DWM_CUI
    if (selmon->cuibar) {
      int tag_end = CUI_BAR_PAD + LENGTH(tags) * CUI_BAR_TAG_WIDTH;
      if (ev->x >= CUI_BAR_PAD && ev->x < tag_end) {
        click = ClkTagBar;
        arg.ui = 1u << ((ev->x - CUI_BAR_PAD) / CUI_BAR_TAG_WIDTH);
      } else if (ev->x >= tag_end && ev->x < tag_end + CUI_BAR_LAYOUT_WIDTH)
        click = ClkLtSymbol;
      else if (cuibarstatuswidth(selmon) && ev->x >= cuibarwidth(selmon) - CUI_BAR_PAD - cuibarstatuswidth(selmon)) {
        if (ev->button == Button1 && CLEANMASK(ev->state) == 0) { dashboardnative(NULL); return; }
        click = ClkStatusText;
      } else click = ClkWinTitle;
    } else
#endif
    {
    i = x = 0;
    for (c = selmon->clients; c; c = c->next)
      occ |= c->tags == TAGMASK ? 0 : c->tags;
    do {
      /* Do not reserve space for vacant tags */
      if (!(occ & 1 << i || selmon->tagset[selmon->seltags] & 1 << i))
        continue;
      x += TEXTW(tags[i]);
    } while (ev->x >= x && ++i < LENGTH(tags));
    if (i < LENGTH(tags)) {
      click = ClkTagBar;
      arg.ui = 1 << i;
    } else if (ev->x < x + TEXTW(selmon->ltsymbol))
      click = ClkLtSymbol;
    else if (ev->x > selmon->ww - (int)TEXTW(stext) + lrpad - 2 -
                         (int)getsystraywidth())
      click = ClkStatusText;
    else
      click = ClkWinTitle;
    }
  } else if ((c = wintoclient(ev->window))) {
    focus(c);
    restack(selmon);
    XAllowEvents(dpy, ReplayPointer, CurrentTime);
    click = ClkClientWin;
  }
  for (i = 0; i < LENGTH(buttons); i++)
    if (click == buttons[i].click && buttons[i].func &&
        buttons[i].button == ev->button &&
        CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
      buttons[i].func(
          click == ClkTagBar && buttons[i].arg.i == 0 ? &arg : &buttons[i].arg);
}
void checkotherwm(void) {
  xerrorxlib = XSetErrorHandler(xerrorstart);
  /* this causes an error if some other window manager is running */
  XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
  XSync(dpy, False);
  XSetErrorHandler(xerror);
  XSync(dpy, False);
}

void cleanup(void) {
  Arg a = {.ui = ~0};
  Layout foo = {"", NULL};
  Monitor *m;
  size_t i;

  view(&a);
  selmon->lt[selmon->sellt] = &foo;
  for (m = mons; m; m = m->next)
    while (m->stack)
      unmanage(m->stack, 0);
  XUngrabKey(dpy, AnyKey, AnyModifier, root);
  while (mons)
    cleanupmon(mons);
  if (showsystray && systray) {
    XUnmapWindow(dpy, systray->win);
    XDestroyWindow(dpy, systray->win);
    free(systray);
    systray = NULL;
  }
  for (i = 0; i < CurLast; i++)
    drw_cur_free(drw, cursor[i]);
  for (i = 0; i < LENGTH(colors) + 1; i++)
    drw_scm_free(drw, scheme[i], 3);
  free(scheme);
  XDestroyWindow(dpy, wmcheckwin);
  drw_free(drw);
#ifdef DWM_CUI
  if (bar_ui) cui_destroy(bar_ui);
  if (bar_renderer) cui_sw_destroy(bar_renderer);
#endif
  XSync(dpy, False);
  XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
  XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}

void cleanupmon(Monitor *mon) {
  Monitor *m;

  if (mon == mons)
    mons = mons->next;
  else {
    for (m = mons; m && m->next != mon; m = m->next)
      ;
    m->next = mon->next;
  }
  XUnmapWindow(dpy, mon->barwin);
  XDestroyWindow(dpy, mon->barwin);
  free(mon);
}

void clientmessage(XEvent *e) {
  XWindowAttributes wa;
  XSetWindowAttributes swa;
  XClientMessageEvent *cme = &e->xclient;
  Client *c = wintoclient(cme->window);

  if (showsystray && systray && cme->window == systray->win &&
      cme->message_type == netatom[NetSystemTrayOP]) {
    /* add systray icons */
    if (cme->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
      if (!(c = (Client *)calloc(1, sizeof(Client))))
        die("fatal: could not malloc() %u bytes\n", sizeof(Client));
      if (!(c->win = cme->data.l[2])) {
        free(c);
        return;
      }
      c->mon = selmon;
      c->next = systray->icons;
      systray->icons = c;
      if (!XGetWindowAttributes(dpy, c->win, &wa)) {
        /* use sane defaults */
        wa.width = bh;
        wa.height = bh;
        wa.border_width = 0;
      }
      c->x = c->oldx = c->y = c->oldy = 0;
      c->w = c->oldw = wa.width;
      c->h = c->oldh = wa.height;
      c->oldbw = wa.border_width;
      c->bw = 0;
      c->isfloating = True;
      /* reuse tags field as mapped status */
      c->tags = 1;
      updatesizehints(c);
      updatesystrayicongeom(c, wa.width, wa.height);
      XAddToSaveSet(dpy, c->win);
      XSelectInput(dpy, c->win,
                   StructureNotifyMask | PropertyChangeMask |
                       ResizeRedirectMask);
      XReparentWindow(dpy, c->win, systray->win, 0, 0);
      /* use parents background color */
      swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
      XChangeWindowAttributes(dpy, c->win, CWBackPixel, &swa);
      sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime,
                XEMBED_EMBEDDED_NOTIFY, 0, systray->win,
                XEMBED_EMBEDDED_VERSION);
      /* FIXME not sure if I have to send these events, too */
      sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime,
                XEMBED_FOCUS_IN, 0, systray->win, XEMBED_EMBEDDED_VERSION);
      sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime,
                XEMBED_WINDOW_ACTIVATE, 0, systray->win,
                XEMBED_EMBEDDED_VERSION);
      sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime,
                XEMBED_MODALITY_ON, 0, systray->win, XEMBED_EMBEDDED_VERSION);
      XSync(dpy, False);
      resizebarwin(selmon);
      updatesystray();
      setclientstate(c, NormalState);
    }
    return;
  }

  if (!c)
    return;
  if (cme->message_type == netatom[NetWMState]) {
    if (cme->data.l[1] == netatom[NetWMFullscreen] ||
        cme->data.l[2] == netatom[NetWMFullscreen])
      setfullscreen(c, (cme->data.l[0] == 1 /* _NET_WM_STATE_ADD    */
                        || (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */ &&
                            !c->isfullscreen)));
  } else if (cme->message_type == netatom[NetActiveWindow]) {
    if (c != selmon->sel && !c->isurgent)
      seturgent(c, 1);
  }
}

void configure(Client *c) {
  XConfigureEvent ce;

  ce.type = ConfigureNotify;
  ce.display = dpy;
  ce.event = c->win;
  ce.window = c->win;
  ce.x = c->x;
  ce.y = c->y;
  ce.width = c->w;
  ce.height = c->h;
  ce.border_width = c->bw;
  ce.above = None;
  ce.override_redirect = False;
  XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void configurenotify(XEvent *e) {
  Monitor *m;
  Client *c;
  XConfigureEvent *ev = &e->xconfigure;
  int dirty;

  /* TODO: updategeom handling sucks, needs to be simplified */
  if (ev->window == root) {
    dirty = (sw != ev->width || sh != ev->height);
    sw = ev->width;
    sh = ev->height;
    if (updategeom() || dirty) {
      drw_resize(drw, sw, bh);
      updatebars();
      for (m = mons; m; m = m->next) {
        for (c = m->clients; c; c = c->next)
          if (c->isfullscreen)
            resizeclient(c, m->mx, m->my, m->mw, m->mh);
        resizebarwin(m);
      }
      focus(NULL);
      arrange(NULL);
    }
  }
}

void configurerequest(XEvent *e) {
  Client *c;
  Monitor *m;
  XConfigureRequestEvent *ev = &e->xconfigurerequest;
  XWindowChanges wc;

  if ((c = wintoclient(ev->window))) {
    if (ev->value_mask & CWBorderWidth)
      c->bw = ev->border_width;
    else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
      m = c->mon;
      if (ev->value_mask & CWX) {
        c->oldx = c->x;
        c->x = m->mx + ev->x;
      }
      if (ev->value_mask & CWY) {
        c->oldy = c->y;
        c->y = m->my + ev->y;
      }
      if (ev->value_mask & CWWidth) {
        c->oldw = c->w;
        c->w = ev->width;
      }
      if (ev->value_mask & CWHeight) {
        c->oldh = c->h;
        c->h = ev->height;
      }
      if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
        c->x = m->mx + (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
      if ((c->y + c->h) > m->my + m->mh && c->isfloating)
        c->y = m->my + (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
      if ((ev->value_mask & (CWX | CWY)) &&
          !(ev->value_mask & (CWWidth | CWHeight)))
        configure(c);
      if (ISVISIBLE(c))
        XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
    } else
      configure(c);
  } else {
    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.border_width = ev->border_width;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
  }
  XSync(dpy, False);
}

Monitor *createmon(void) {
  Monitor *m;

  m = ecalloc(1, sizeof(Monitor));
  m->tagset[0] = m->tagset[1] = 1;
  m->mfact = mfact;
  m->nmaster = nmaster;
  m->showbar = showbar;
  m->topbar = topbar;
  m->gappx = gappx;
  m->drawwithgaps = startwithgaps;
  m->lt[0] = &layouts[0];
  m->lt[1] = &layouts[1 % LENGTH(layouts)];
  strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol);
  return m;
}

void destroynotify(XEvent *e) {
  Client *c;
  XDestroyWindowEvent *ev = &e->xdestroywindow;

  if ((c = wintoclient(ev->window)))
    unmanage(c, 1);
  else if ((c = wintosystrayicon(ev->window))) {
    removesystrayicon(c);
    resizebarwin(selmon);
    updatesystray();
  }
}

void detach(Client *c) {
  Client **tc;

  for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next)
    ;
  *tc = c->next;
}

void detachstack(Client *c) {
  Client **tc, *t;

  for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext)
    ;
  *tc = c->snext;

  if (c == c->mon->sel) {
    for (t = c->mon->stack; t && !ISVISIBLE(t); t = t->snext)
      ;
    c->mon->sel = t;
  }
}

Monitor *dirtomon(int dir) {
  Monitor *m = NULL;

  if (dir > 0) {
    if (!(m = selmon->next))
      m = mons;
  } else if (selmon == mons)
    for (m = mons; m->next; m = m->next)
      ;
  else
    for (m = mons; m->next != selmon; m = m->next)
      ;
  return m;
}

int drawstatusbar(Monitor *m, int bh, char *stext) {
  int ret, i, w, x, len;
  short isCode = 0;
  char *text;
  char *p;

  len = strlen(stext) + 1;
  if (!(text = (char *)malloc(sizeof(char) * len)))
    die("malloc");
  p = text;
  memcpy(text, stext, len);

  /* compute width of the status text */
  w = 0;
  i = -1;
  while (text[++i]) {
    if (text[i] == '^') {
      if (!isCode) {
        isCode = 1;
        text[i] = '\0';
        w += TEXTW(text) - lrpad;
        text[i] = '^';
        if (text[++i] == 'f')
          w += atoi(text + ++i);
      } else {
        isCode = 0;
        text = text + i + 1;
        i = -1;
      }
    }
  }
  if (!isCode)
    w += TEXTW(text) - lrpad;
  else
    isCode = 0;
  text = p;

  w += 2; /* 1px padding on both sides */
  ret = m->ww - w;
  x = m->ww - w - getsystraywidth();

  drw_setscheme(drw, scheme[LENGTH(colors)]);
  drw->scheme[ColFg] = scheme[SchemeNorm][ColFg];
  drw->scheme[ColBg] = scheme[SchemeNorm][ColBg];
  drw_rect(drw, x, 0, w, bh, 1, 1);
  x++;

  /* process status text */
  i = -1;
  while (text[++i]) {
    if (text[i] == '^' && !isCode) {
      isCode = 1;

      text[i] = '\0';
      w = TEXTW(text) - lrpad;
      drw_text(drw, x, 0, w, bh, 0, text, 0);

      x += w;

      /* process code */
      while (text[++i] != '^') {
        if (text[i] == 'c') {
          char buf[8];
          memcpy(buf, (char *)text + i + 1, 7);
          buf[7] = '\0';
          drw_clr_create(drw, &drw->scheme[ColFg], buf);
          i += 7;
        } else if (text[i] == 'b') {
          char buf[8];
          memcpy(buf, (char *)text + i + 1, 7);
          buf[7] = '\0';
          drw_clr_create(drw, &drw->scheme[ColBg], buf);
          i += 7;
        } else if (text[i] == 'd') {
          drw->scheme[ColFg] = scheme[SchemeNorm][ColFg];
          drw->scheme[ColBg] = scheme[SchemeNorm][ColBg];
        } else if (text[i] == 'r') {
          int rx = atoi(text + ++i);
          while (text[++i] != ',')
            ;
          int ry = atoi(text + ++i);
          while (text[++i] != ',')
            ;
          int rw = atoi(text + ++i);
          while (text[++i] != ',')
            ;
          int rh = atoi(text + ++i);

          drw_rect(drw, rx + x, ry, rw, rh, 1, 0);
        } else if (text[i] == 'f') {
          x += atoi(text + ++i);
        }
      }

      text = text + i + 1;
      i = -1;
      isCode = 0;
    }
  }

  if (!isCode) {
    w = TEXTW(text) - lrpad;
    drw_text(drw, x, 0, w, bh, 0, text, 0);
  }

  drw_setscheme(drw, scheme[SchemeNorm]);
  free(p);

  return ret;
}

#ifdef DWM_CUI
static int
cuibarwidth(Monitor *m)
{
  int tray = showsystray && m == systraytomon(m) && !systrayonleft ? (int)getsystraywidth() : 0;
  return MAX(1, m->ww - tray);
}

static int
cuibarstatuswidth(Monitor *m)
{
  int width = cuibarwidth(m);
  int left = 2 * CUI_BAR_PAD + LENGTH(tags) * CUI_BAR_TAG_WIDTH + CUI_BAR_LAYOUT_WIDTH;
  return m == selmon ? MAX(0, MIN(240, width - left - 80)) : 0;
}

static int
cuibardraw(Monitor *m)
{
  XImage *image;
  unsigned int i, occupied = 0, urgent = 0;
  int width = cuibarwidth(m), statuswidth = cuibarstatuswidth(m), in_markup = 0;
  char number[8], status[sizeof stext];
  size_t j, used = 0;
  Client *c;
  cui_theme *theme;
  cui_renderer_iface iface;
  if (!bar_renderer) {
    bar_renderer = cui_sw_create();
    if (!bar_renderer) return 0;
    cui_sw_set_threads(bar_renderer, 1);
    iface = cui_sw_iface(bar_renderer);
    bar_ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = CUI_THEME_DARK });
    if (!bar_ui) { cui_sw_destroy(bar_renderer); bar_renderer = NULL; return 0; }
    cui_font_set_role(bar_ui, CUI_FONT_BODY, CUI_FONT_MONO);
    theme = cui_theme_get(bar_ui);
    theme->colors[CUI_TOKEN_BACKGROUND] = CUI_RGB(0x090b0d);
    theme->colors[CUI_TOKEN_FOREGROUND] = CUI_RGB(0xadbac7);
  }
  /* The software renderer's BGRA format matches common 24-bit X11 visuals.
   * On other visuals, keep the original bar instead of corrupting colors. */
  image = XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
                       ZPixmap, 0, NULL, width, bh, 32, 0);
  if (!image) return 0;
  if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst ||
      image->red_mask != 0xff0000 || image->green_mask != 0xff00 || image->blue_mask != 0xff) {
    XDestroyImage(image); return 0;
  }
  image->data = calloc((size_t)image->bytes_per_line, (size_t)bh);
  if (!image->data) { XDestroyImage(image); return 0; }
  for (c = m->clients; c; c = c->next) {
    occupied |= c->tags == TAGMASK ? 0 : c->tags;
    if (c->isurgent) urgent |= c->tags;
  }
  /* status2d escape sequences are for the old renderer; omit them in CUI. */
  for (j = 0; stext[j] && used + 1 < sizeof status; j++) {
    unsigned char ch = (unsigned char)stext[j];
    if (ch == '^') { in_markup = !in_markup; continue; }
    if (!in_markup && ch >= 32 && ch != 127) status[used++] = (char)ch;
  }
  status[used] = '\0';
  cui_set_viewport(bar_ui, (float)width, (float)bh, 1.0f);
  cui_begin_frame(bar_ui, 0.016f);
  cui_row(bar_ui, .width = CUI_FILL, .height = CUI_FILL, .pad_x = CUI_BAR_PAD,
          .gap = 0, .align = CUI_ALIGN_CENTER, .bg = CUI_RGB(0x090b0d), .clip = true) {
    for (i = 0; i < LENGTH(tags); i++) {
      int active = !!(m->tagset[m->seltags] & (1u << i));
      cui_color bg = active ? CUI_RGB(0x2d3035) : CUI_RGB(0x090b0d);
      cui_color fg = urgent & (1u << i) ? CUI_RGB(0xb86a6a) : active ? CUI_RGB(0xf2d49e) :
                     occupied & (1u << i) ? CUI_RGB(0xadbac7) : CUI_RGB(0x64778d);
      snprintf(number, sizeof number, "%u", i + 1);
      cui_column(bar_ui, .width = CUI_PX(CUI_BAR_TAG_WIDTH), .height = CUI_PX(26),
                 .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .bg = bg, .radius = 6) {
        cui_text(bar_ui, number, .font = CUI_FONT_MONO, .size = 12, .color = fg);
      }
    }
    cui_column(bar_ui, .width = CUI_PX(CUI_BAR_LAYOUT_WIDTH), .height = CUI_FILL,
               .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
      cui_text(bar_ui, m->ltsymbol, .font = CUI_FONT_MONO, .size = 12, .color = CUI_RGB(0x7fa8c9));
    }
    cui_column(bar_ui, .width = CUI_FILL, .height = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .clip = true) {
      cui_text(bar_ui, m->sel ? m->sel->name : "Desktop", .font = CUI_FONT_MONO,
               .size = 12, .color = CUI_RGB(0x8a949e), .width = CUI_FILL, .nowrap = true);
    }
    if (statuswidth > 0) {
      cui_column(bar_ui, .width = CUI_PX(statuswidth), .height = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .clip = true) {
        cui_text(bar_ui, status, .font = CUI_FONT_MONO, .size = 12, .color = CUI_RGB(0xadbac7),
                 .width = CUI_FILL, .nowrap = true, .align = CUI_TEXT_RIGHT);
      }
    }
  }
  cui_end_frame(bar_ui);
  cui_sw_render(bar_renderer, cui_get_draw_data(bar_ui), image->data, width, bh, image->bytes_per_line, CUI_SW_BGRA);
  XPutImage(dpy, m->barwin, drw->gc, image, 0, 0, 0, 0, width, bh);
  XDestroyImage(image);
  return 1;
}
#endif

void drawbar(Monitor *m) {
  int x, w, tw = 0, stw = 0;
  int boxs = drw->fonts->h / 9;
  int boxw = drw->fonts->h / 6 + 2;
  unsigned int i, occ = 0, urg = 0;
  Client *c;

  if (!m->showbar)
    return;
#ifdef DWM_CUI
  resizebarwin(m);
  if ((m->cuibar = cuibardraw(m))) return;
#endif

  if (showsystray && m == systraytomon(m) && !systrayonleft)
    stw = getsystraywidth();

  /* draw status first so it can be overdrawn by tags later */
  if (m == selmon) { /* status is only drawn on selected monitor */
    tw = m->ww - drawstatusbar(m, bh, stext);
  }

  resizebarwin(m);
  for (c = m->clients; c; c = c->next) {
    occ |= c->tags == TAGMASK ? 0 : c->tags;
    if (c->isurgent)
      urg |= c->tags;
  }
  x = 0;
  for (i = 0; i < LENGTH(tags); i++) {
    /* Do not draw vacant tags */
    if (!(occ & 1 << i || m->tagset[m->seltags] & 1 << i))
      continue;
    w = TEXTW(tags[i]);
    drw_setscheme(
        drw, scheme[m->tagset[m->seltags] & 1 << i ? SchemeSel : SchemeNorm]);
    drw_text(drw, x, 0, w, bh, lrpad / 2, tags[i], urg & 1 << i);
    x += w;
  }
  w = TEXTW(m->ltsymbol);
  drw_setscheme(drw, scheme[SchemeNorm]);
  x = drw_text(drw, x, 0, w, bh, lrpad / 2, m->ltsymbol, 0);

  if ((w = m->ww - tw - stw - x) > bh) {
    if (m->sel) {
      drw_setscheme(drw, scheme[m == selmon ? SchemeSel : SchemeNorm]);
      drw_text(drw, x, 0, w, bh, lrpad / 2, m->sel->name, 0);
      if (m->sel->isfloating)
        drw_rect(drw, x + boxs, boxs, boxw, boxw, m->sel->isfixed, 0);
    } else {
      drw_setscheme(drw, scheme[SchemeNorm]);
      drw_rect(drw, x, 0, w, bh, 1, 1);
    }
  }
  drw_map(drw, m->barwin, 0, 0, m->ww - stw, bh);
}

void drawbars(void) {
  Monitor *m;

  for (m = mons; m; m = m->next)
    drawbar(m);
}

void enternotify(XEvent *e) {
  Client *c;
  Monitor *m;
  XCrossingEvent *ev = &e->xcrossing;

  if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) &&
      ev->window != root)
    return;
  c = wintoclient(ev->window);
  m = c ? c->mon : wintomon(ev->window);
  if (m != selmon) {
    unfocus(selmon->sel, 1);
    selmon = m;
  } else if (!c || c == selmon->sel)
    return;
  focus(c);
}

void expose(XEvent *e) {
  Monitor *m;
  XExposeEvent *ev = &e->xexpose;

  if (ev->count == 0 && (m = wintomon(ev->window))) {
    drawbar(m);
    if (m == selmon)
      updatesystray();
  }
}

void focus(Client *c) {
  if (!c || !ISVISIBLE(c))
    for (c = selmon->stack; c && !ISVISIBLE(c); c = c->snext)
      ;
  if (selmon->sel && selmon->sel != c)
    unfocus(selmon->sel, 0);
  if (c) {
    if (c->mon != selmon)
      selmon = c->mon;
    if (c->isurgent)
      seturgent(c, 0);
    detachstack(c);
    attachstack(c);
    grabbuttons(c, 1);
    XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);
    if (!selmon->drawwithgaps && !c->isfloating) {
      XWindowChanges wc;
      wc.sibling = selmon->barwin;
      wc.stack_mode = Below;
      XConfigureWindow(dpy, c->win, CWSibling | CWStackMode, &wc);
    }
    setfocus(c);
  } else {
    XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
  }
  selmon->sel = c;
  drawbars();
}

/* there are some broken focus acquiring clients needing extra handling */
void focusin(XEvent *e) {
  XFocusChangeEvent *ev = &e->xfocus;

  if (selmon->sel && ev->window != selmon->sel->win)
    setfocus(selmon->sel);
}

void focusmon(const Arg *arg) {
  Monitor *m;

  if (!mons->next)
    return;
  if ((m = dirtomon(arg->i)) == selmon)
    return;
  unfocus(selmon->sel, 0);
  selmon = m;
  focus(NULL);
  warp(selmon->sel);
}
void focusdir(const Arg *arg) {
  Client *s = selmon->sel, *f = NULL, *c, *next;
  unsigned int score = -1, client_score;
  int dist, dirweight = 20;

  if (!s)
    return;

  next = s->next;
  if (!next)
    next = s->mon->clients;
  for (c = next; c != s; c = next) {
    next = c->next;
    if (!next)
      next = s->mon->clients;

    if (!ISVISIBLE(c) || c->isfloating != s->isfloating)
      continue;

    switch (arg->i) {
    case 0: /* left */
      dist = s->x - c->x - c->w;
      client_score =
          dirweight * MIN(abs(dist), abs(dist + s->mon->mw)) + abs(s->y - c->y);
      break;
    case 1: /* right */
      dist = c->x - s->x - s->w;
      client_score =
          dirweight * MIN(abs(dist), abs(dist + s->mon->mw)) + abs(c->y - s->y);
      break;
    case 2: /* up */
      dist = s->y - c->y - c->h;
      client_score =
          dirweight * MIN(abs(dist), abs(dist + s->mon->mh)) + abs(s->x - c->x);
      break;
    default: /* down */
      dist = c->y - s->y - s->h;
      client_score =
          dirweight * MIN(abs(dist), abs(dist + s->mon->mh)) + abs(c->x - s->x);
      break;
    }

    if (client_score < score) {
      score = client_score;
      f = c;
    }
  }

  if (f && f != s) {
    focus(f);
    restack(f->mon);
    warp(f);
  }
}

void focusstack(const Arg *arg) {
  Client *c = NULL, *i;

  if (!selmon->sel || (selmon->sel->isfullscreen && lockfullscreen))
    return;
  if (arg->i > 0) {
    for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next)
      ;
    if (!c)
      for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next)
        ;
  } else {
    for (i = selmon->clients; i != selmon->sel; i = i->next)
      if (ISVISIBLE(i))
        c = i;
    if (!c)
      for (; i; i = i->next)
        if (ISVISIBLE(i))
          c = i;
  }
  if (c) {
    focus(c);
    restack(selmon);
    warp(c);
  }
}

Atom getatomprop(Client *c, Atom prop) {
  int format;
  unsigned long nitems, dl;
  unsigned char *p = NULL;
  Atom da, atom = None;

  /* FIXME getatomprop should return the number of items and a pointer to
   * the stored data instead of this workaround */
  Atom req = XA_ATOM;
  if (prop == xatom[XembedInfo])
    req = xatom[XembedInfo];

  if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, req, &da,
                         &format, &nitems, &dl, &p) == Success &&
      p) {
    if (nitems > 0 && format == 32)
      atom = *(long *)p;
    if (da == xatom[XembedInfo] && nitems == 2)
      atom = ((long *)p)[1];
    XFree(p);
  }
  return atom;
}

int getrootptr(int *x, int *y) {
  int di;
  unsigned int dui;
  Window dummy;

  return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long getstate(Window w) {
  int format;
  long result = -1;
  unsigned char *p = NULL;
  unsigned long n, extra;
  Atom real;

  if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False,
                         wmatom[WMState], &real, &format, &n, &extra,
                         &p) != Success)
    return -1;
  if (n != 0 && format == 32)
    result = *(long *)p;
  XFree(p);
  return result;
}

unsigned int getsystraywidth() {
  unsigned int w = 0;
  Client *i;
  if (showsystray && systray)
    for (i = systray->icons; i; w += i->w + systrayspacing, i = i->next)
      ;
  return w ? w + systrayspacing : 1;
}

int gettextprop(Window w, Atom atom, char *text, unsigned int size) {
  char **list = NULL;
  int n;
  XTextProperty name;

  if (!text || size == 0)
    return 0;
  text[0] = '\0';
  if (!XGetTextProperty(dpy, w, &name, atom) || !name.nitems)
    return 0;
  if (name.encoding == XA_STRING) {
    strncpy(text, (char *)name.value, size - 1);
  } else if (XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success &&
             n > 0 && *list) {
    strncpy(text, *list, size - 1);
    XFreeStringList(list);
  }
  text[size - 1] = '\0';
  XFree(name.value);
  return 1;
}

void grabbuttons(Client *c, int focused) {
  updatenumlockmask();
  {
    unsigned int i, j;
    unsigned int modifiers[] = {0, LockMask, numlockmask,
                                numlockmask | LockMask};
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    if (!focused)
      XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, BUTTONMASK,
                  GrabModeSync, GrabModeSync, None, None);
    for (i = 0; i < LENGTH(buttons); i++)
      if (buttons[i].click == ClkClientWin)
        for (j = 0; j < LENGTH(modifiers); j++)
          XGrabButton(dpy, buttons[i].button, buttons[i].mask | modifiers[j],
                      c->win, False, BUTTONMASK, GrabModeAsync, GrabModeSync,
                      None, None);
  }
}

/* setxkbmap publishes five NUL-separated strings on the root window. If
 * XKB already owns Win+Space, leave the key ungrabbed so it cannot toggle
 * twice. No existing keyboard options or startup files are modified. */
static int
xkbwinspaceoption(const unsigned char *data, size_t length)
{
  size_t start = 0, end;
  int field;
  for (field = 0; field < 4; field++) {
    while (start < length && data[start]) start++;
    if (start == length) return 0;
    start++;
  }
  while (start < length && data[start]) {
    end = start;
    while (end < length && data[end] && data[end] != ',') end++;
    if (end - start == sizeof("grp:win_space_toggle") - 1 &&
        !memcmp(data + start, "grp:win_space_toggle", end - start)) return 1;
    if (end == length || !data[end]) break;
    start = end + 1;
  }
  return 0;
}

static int
xkbownswinspace(void)
{
  Atom actual;
  int format, found = 0;
  unsigned long nitems, remaining;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, root, xkbrules, 0, 4096, False, XA_STRING,
                         &actual, &format, &nitems, &remaining, &data) == Success &&
      actual == XA_STRING && format == 8 && data)
    found = xkbwinspaceoption(data, nitems);
  if (data) XFree(data);
  return found;
}

void grabkeys(void) {
  updatenumlockmask();
  {
    unsigned int i, j, k;
    unsigned int modifiers[] = {0, LockMask, numlockmask,
                                numlockmask | LockMask};
    int start, end, skip, xkb_owns_win_space = xkbownswinspace();
    KeySym *syms;

    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XDisplayKeycodes(dpy, &start, &end);
    syms = XGetKeyboardMapping(dpy, start, end - start + 1, &skip);
    if (!syms)
      return;
    for (k = start; k <= end; k++)
      for (i = 0; i < LENGTH(keys); i++)
        /* skip modifier codes, we do that ourselves */
        if (!(xkb_owns_win_space && keys[i].func == togglekeyboardgroup) &&
            keys[i].keysym == syms[(k - start) * skip])
          for (j = 0; j < LENGTH(modifiers); j++)
            XGrabKey(dpy, k, keys[i].mod | modifiers[j], root, True,
                     GrabModeAsync, GrabModeAsync);
    XFree(syms);
  }
}

void incnmaster(const Arg *arg) {
  selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
  arrange(selmon);
}

#ifdef XINERAMA
static int isuniquegeom(XineramaScreenInfo *unique, size_t n,
                        XineramaScreenInfo *info) {
  while (n--)
    if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org &&
        unique[n].width == info->width && unique[n].height == info->height)
      return 0;
  return 1;
}
#endif /* XINERAMA */

void keypress(XEvent *e) {
  unsigned int i;
  KeySym keysym;
  XKeyEvent *ev;

  ev = &e->xkey;
  keysym = XKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0);
  for (i = 0; i < LENGTH(keys); i++)
    if (keysym == keys[i].keysym &&
        CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
      keys[i].func(&(keys[i].arg));
}

/* Use the same close protocol for the focused window and switcher entries. */
void closeclient(Client *c) {
  if (!c)
    return;
  if (!sendevent(c->win, wmatom[WMDelete], NoEventMask,
                 wmatom[WMDelete], CurrentTime, 0, 0, 0)) {
    XGrabServer(dpy);
    XSetErrorHandler(xerrordummy);
    XSetCloseDownMode(dpy, DestroyAll);
    XKillClient(dpy, c->win);
    XSync(dpy, False);
    XSetErrorHandler(xerror);
    XUngrabServer(dpy);
  }
}

void killclient(const Arg *arg) {
  closeclient(selmon->sel);
}

void manage(Window w, XWindowAttributes *wa) {
  Client *c, *t = NULL;
  Window trans = None;
  XWindowChanges wc;

  c = ecalloc(1, sizeof(Client));
  c->win = w;
  /* geometry */
  c->x = c->oldx = wa->x;
  c->y = c->oldy = wa->y;
  c->w = c->oldw = wa->width;
  c->h = c->oldh = wa->height;
  c->oldbw = wa->border_width;

  updatetitle(c);
  if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
    c->mon = t->mon;
    c->tags = t->tags;
  } else {
    c->mon = selmon;
    applyrules(c);
  }

  if (c->x + WIDTH(c) > c->mon->wx + c->mon->ww)
    c->x = c->mon->wx + c->mon->ww - WIDTH(c);
  if (c->y + HEIGHT(c) > c->mon->wy + c->mon->wh)
    c->y = c->mon->wy + c->mon->wh - HEIGHT(c);
  c->x = MAX(c->x, c->mon->wx);
  c->y = MAX(c->y, c->mon->wy);
  c->bw = borderpx;

  wc.border_width = c->bw;
  XConfigureWindow(dpy, w, CWBorderWidth, &wc);
  XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);
  configure(c); /* propagates border_width, if size doesn't change */
  updatewindowtype(c);
  updatesizehints(c);
  updatewmhints(c);
  c->x = c->mon->mx + (c->mon->mw - WIDTH(c)) / 2;
  c->y = c->mon->my + (c->mon->mh - HEIGHT(c)) / 2;
  XSelectInput(dpy, w,
               EnterWindowMask | FocusChangeMask | PropertyChangeMask |
                   StructureNotifyMask);
  grabbuttons(c, 0);
  if (!c->isfloating)
    c->isfloating = c->oldstate = trans != None || c->isfixed;
  if (c->isfloating)
    XRaiseWindow(dpy, c->win);
  attach(c);
  attachstack(c);
  XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
                  PropModeAppend, (unsigned char *)&(c->win), 1);
  XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w,
                    c->h); /* some windows require this */
  setclientstate(c, NormalState);
  if (c->mon == selmon)
    unfocus(selmon->sel, 0);
  c->mon->sel = c;
  arrange(c->mon);
  XMapWindow(dpy, c->win);
  focus(NULL);
}

void mappingnotify(XEvent *e) {
  XMappingEvent *ev = &e->xmapping;

  XRefreshKeyboardMapping(ev);
  if (ev->request == MappingKeyboard || ev->request == MappingModifier)
    grabkeys();
}

void maprequest(XEvent *e) {
  static XWindowAttributes wa;
  XMapRequestEvent *ev = &e->xmaprequest;

  Client *i;
  if ((i = wintosystrayicon(ev->window))) {
    sendevent(i->win, netatom[Xembed], StructureNotifyMask, CurrentTime,
              XEMBED_WINDOW_ACTIVATE, 0, systray->win, XEMBED_EMBEDDED_VERSION);
    resizebarwin(selmon);
    updatesystray();
  }

  if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
    return;
  if (!wintoclient(ev->window))
    manage(ev->window, &wa);
}

void monocle(Monitor *m) {
  unsigned int n = 0;
  Client *c;

  for (c = m->clients; c; c = c->next)
    if (ISVISIBLE(c))
      n++;
  if (n > 0) /* override layout symbol */
    snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
  for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
    if (selmon->drawwithgaps)
      resize(c, m->wx, m->wy, m->ww - 2 * c->bw, m->wh - 2 * c->bw, 0);
    else
      resize(c, m->wx - c->bw, m->wy, m->ww, m->wh, False);
}

void motionnotify(XEvent *e) {
  static Monitor *mon = NULL;
  Monitor *m;
  XMotionEvent *ev = &e->xmotion;

  if (ev->window != root)
    return;
  if ((m = recttomon(ev->x_root, ev->y_root, 1, 1)) != mon && mon) {
    unfocus(selmon->sel, 1);
    selmon = m;
    focus(NULL);
  }
  mon = m;
}

void movemouse(const Arg *arg) {
  int x, y, ocx, ocy, nx, ny;
  Client *c;
  Monitor *m;
  XEvent ev;
  Time lasttime = 0;

  if (!(c = selmon->sel))
    return;
  if (c->isfullscreen) /* no support moving fullscreen windows by mouse */
    return;
  restack(selmon);
  ocx = c->x;
  ocy = c->y;
  if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
                   None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
    return;
  if (!getrootptr(&x, &y))
    return;
  do {
    XMaskEvent(dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask, &ev);
    switch (ev.type) {
    case ConfigureRequest:
    case Expose:
    case MapRequest:
      handler[ev.type](&ev);
      break;
    case MotionNotify:
      if ((ev.xmotion.time - lasttime) <= (1000 / refreshrate))
        continue;
      lasttime = ev.xmotion.time;

      nx = ocx + (ev.xmotion.x - x);
      ny = ocy + (ev.xmotion.y - y);
      if (abs(selmon->wx - nx) < snap)
        nx = selmon->wx;
      else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < snap)
        nx = selmon->wx + selmon->ww - WIDTH(c);
      if (abs(selmon->wy - ny) < snap)
        ny = selmon->wy;
      else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < snap)
        ny = selmon->wy + selmon->wh - HEIGHT(c);
      if (!c->isfloating && selmon->lt[selmon->sellt]->arrange &&
          (abs(nx - c->x) > snap || abs(ny - c->y) > snap))
        togglefloating(NULL);
      if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
        resize(c, nx, ny, c->w, c->h, 1);
      break;
    }
  } while (ev.type != ButtonRelease);
  XUngrabPointer(dpy, CurrentTime);
  if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
    sendmon(c, m);
    selmon = m;
    focus(NULL);
  }
}

Client *nexttiled(Client *c) {
  for (; c && (c->isfloating || !ISVISIBLE(c)); c = c->next)
    ;
  return c;
}

void pop(Client *c) {
  detach(c);
  attach(c);
  focus(c);
  arrange(c->mon);
}

void propertynotify(XEvent *e) {
  Client *c;
  Window trans;
  XPropertyEvent *ev = &e->xproperty;

  if ((c = wintosystrayicon(ev->window))) {
    if (ev->atom == XA_WM_NORMAL_HINTS) {
      updatesizehints(c);
      updatesystrayicongeom(c, c->w, c->h);
    } else
      updatesystrayiconstate(c, ev);
    resizebarwin(selmon);
    updatesystray();
  }

  if (ev->window == root && ev->atom == xkbrules) {
    grabkeys();
    return;
  }
  if ((ev->window == root) && (ev->atom == XA_WM_NAME))
    updatestatus();
  else if (ev->state == PropertyDelete)
    return; /* ignore */
  else if ((c = wintoclient(ev->window))) {
    switch (ev->atom) {
    default:
      break;
    case XA_WM_TRANSIENT_FOR:
      if (!c->isfloating && (XGetTransientForHint(dpy, c->win, &trans)) &&
          (c->isfloating = (wintoclient(trans)) != NULL))
        arrange(c->mon);
      break;
    case XA_WM_NORMAL_HINTS:
      c->hintsvalid = 0;
      break;
    case XA_WM_HINTS:
      updatewmhints(c);
      drawbars();
      break;
    }
    if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
      updatetitle(c);
      if (c == c->mon->sel)
        drawbar(c->mon);
    }
    if (ev->atom == netatom[NetWMWindowType])
      updatewindowtype(c);
  }
}

void quit(const Arg *arg) { running = 0; }

Monitor *recttomon(int x, int y, int w, int h) {
  Monitor *m, *r = selmon;
  int a, area = 0;

  for (m = mons; m; m = m->next)
    if ((a = INTERSECT(x, y, w, h, m)) > area) {
      area = a;
      r = m;
    }
  return r;
}

void removesystrayicon(Client *i) {
  Client **ii;

  if (!showsystray || !i)
    return;
  for (ii = &systray->icons; *ii && *ii != i; ii = &(*ii)->next)
    ;
  if (ii)
    *ii = i->next;
  free(i);
}

void resize(Client *c, int x, int y, int w, int h, int interact) {
  if (applysizehints(c, &x, &y, &w, &h, interact))
    resizeclient(c, x, y, w, h);
}

void resizebarwin(Monitor *m) {
  unsigned int w = m->ww;
  if (showsystray && m == systraytomon(m) && !systrayonleft)
    w -= getsystraywidth();
  XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, w, bh);
}

void resizeclient(Client *c, int x, int y, int w, int h) {
  XWindowChanges wc;

  c->oldx = c->x;
  c->x = wc.x = x;
  c->oldy = c->y;
  c->y = wc.y = y;
  c->oldw = c->w;
  c->w = wc.width = w;
  c->oldh = c->h;
  c->h = wc.height = h;
  wc.border_width = c->bw;
  if (!selmon->drawwithgaps && /* this is the noborderfloatingfix patch,
                                  slightly modified so that it will work if, and
                                  only if, gaps are disabled. */
      (((nexttiled(c->mon->clients) == c &&
         !nexttiled(c->next)) /* these two first lines are the only ones
                                 changed. if you are manually patching and have
                                 noborder installed already, just change these
                                 lines; or conversely, just remove this section
                                 if the noborder patch is not desired ;) */
        || &monocle == c->mon->lt[c->mon->sellt]->arrange)) &&
      !c->isfullscreen && !c->isfloating &&
      NULL != c->mon->lt[c->mon->sellt]->arrange) {
    c->w = wc.width += c->bw * 2;
    c->h = wc.height += c->bw * 2;
    wc.border_width = 0;
  }
  XConfigureWindow(dpy, c->win, CWX | CWY | CWWidth | CWHeight | CWBorderWidth,
                   &wc);
  configure(c);
  XSync(dpy, False);
}

void resizemouse(const Arg *arg) {
  int ocx, ocy, nw, nh;
  Client *c;
  Monitor *m;
  XEvent ev;
  Time lasttime = 0;

  if (!(c = selmon->sel))
    return;
  if (c->isfullscreen) /* no support resizing fullscreen windows by mouse */
    return;
  restack(selmon);
  ocx = c->x;
  ocy = c->y;
  if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
                   None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
    return;
  XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1,
               c->h + c->bw - 1);
  do {
    XMaskEvent(dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask, &ev);
    switch (ev.type) {
    case ConfigureRequest:
    case Expose:
    case MapRequest:
      handler[ev.type](&ev);
      break;
    case MotionNotify:
      if ((ev.xmotion.time - lasttime) <= (1000 / refreshrate))
        continue;
      lasttime = ev.xmotion.time;

      nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
      nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);
      if (c->mon->wx + nw >= selmon->wx &&
          c->mon->wx + nw <= selmon->wx + selmon->ww &&
          c->mon->wy + nh >= selmon->wy &&
          c->mon->wy + nh <= selmon->wy + selmon->wh) {
        if (!c->isfloating && selmon->lt[selmon->sellt]->arrange &&
            (abs(nw - c->w) > snap || abs(nh - c->h) > snap))
          togglefloating(NULL);
      }
      if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
        resize(c, c->x, c->y, nw, nh, 1);
      break;
    }
  } while (ev.type != ButtonRelease);
  XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1,
               c->h + c->bw - 1);
  XUngrabPointer(dpy, CurrentTime);
  while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
    ;
  if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
    sendmon(c, m);
    selmon = m;
    focus(NULL);
  }
}

void resizerequest(XEvent *e) {
  XResizeRequestEvent *ev = &e->xresizerequest;
  Client *i;

  if ((i = wintosystrayicon(ev->window))) {
    updatesystrayicongeom(i, ev->width, ev->height);
    resizebarwin(selmon);
    updatesystray();
  }
}

void restack(Monitor *m) {
  Client *c;
  XEvent ev;
  XWindowChanges wc;

  drawbar(m);
  if (!m->sel)
    return;
  if (m->sel->isfloating || !m->lt[m->sellt]->arrange)
    XRaiseWindow(dpy, m->sel->win);
  if (m->lt[m->sellt]->arrange) {
    wc.stack_mode = Below;
    wc.sibling = m->barwin;
    for (c = m->stack; c; c = c->snext)
      if (!c->isfloating && ISVISIBLE(c)) {
        XConfigureWindow(dpy, c->win, CWSibling | CWStackMode, &wc);
        wc.sibling = c->win;
      }
  }
  XSync(dpy, False);
  while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
    ;
}

void run(void) {
  XEvent ev;
  /* main event loop */
  XSync(dpy, False);
  while (running && !XNextEvent(dpy, &ev))
    if (ev.type < LASTEvent && handler[ev.type])
      handler[ev.type](&ev); /* call handler */
}

/* Create parent directories but never change existing files or permissions. */
static int
mkdirparents(const char *path)
{
  char dir[PATH_MAX], *p;
  size_t len = strlen(path);
  if (!len || len >= sizeof dir) { errno = ENAMETOOLONG; return 0; }
  memcpy(dir, path, len + 1);
  for (p = dir + (dir[0] == '/'); ; p++) {
    if (*p == '/' || *p == '\0') {
      char end = *p;
      *p = '\0';
      if (*dir && mkdir(dir, 0700) < 0 && errno != EEXIST) return 0;
      *p = end;
      if (!end) break;
    }
  }
  return 1;
}

/* Publish a fully written file using link(2), which fails rather than
 * replacing an existing file, including a symlink or another process's file. */
static int
createifmissing(const char *dir, const char *name, const char *contents, mode_t mode)
{
  char path[PATH_MAX], tmp[PATH_MAX];
  size_t remaining = strlen(contents);
  const char *p = contents;
  int fd, ok = 1, saved;
  ssize_t n;
  if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path ||
      snprintf(tmp, sizeof tmp, "%s/.dwm-init-XXXXXX", dir) >= (int)sizeof tmp) {
    errno = ENAMETOOLONG; return 0;
  }
  /* Fast path is optional; link() below is the authoritative race check. */
  if (lstat(path, &(struct stat){0}) == 0 || errno == EEXIST) return 1;
  if (errno != ENOENT) return 0;
  if ((fd = mkstemp(tmp)) < 0) return 0;
  while (remaining) {
    n = write(fd, p, remaining);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = 0; break; }
    p += n; remaining -= (size_t)n;
  }
  if (ok && fchmod(fd, mode) < 0) ok = 0;
  saved = errno;
  if (close(fd) < 0 && ok) { ok = 0; saved = errno; }
  if (ok && link(tmp, path) < 0 && errno != EEXIST) { ok = 0; saved = errno; }
  unlink(tmp);
  if (!ok) errno = saved;
  return ok;
}

/* Start executable autostart files without shell interpolation of their paths.
 * The blocking variant temporarily enables waitpid, then restores dwm's
 * normal SIGCHLD policy. Existing non-executable files are left alone. */
static void
startautostart(const char *path, int blocking)
{
  struct sigaction old, normal;
  pid_t child;
  int status;
  if (access(path, X_OK) != 0) return;
  if (blocking) {
    memset(&normal, 0, sizeof normal);
    normal.sa_handler = SIG_DFL;
    sigemptyset(&normal.sa_mask);
    if (sigaction(SIGCHLD, &normal, &old) < 0) return;
  }
  child = fork();
  if (child == 0) {
    close(ConnectionNumber(dpy));
    signal(SIGCHLD, SIG_DFL);
    if (!blocking) setsid();
    execl(path, path, (char *)NULL);
    if (errno == ENOEXEC) execl("/bin/sh", "sh", path, (char *)NULL);
    fprintf(stderr, "dwm: could not start %s: %s\n", path, strerror(errno));
    _exit(127);
  }
  if (child < 0) fprintf(stderr, "dwm: cannot start %s: %s\n", path, strerror(errno));
  else if (blocking) {
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) ;
  }
  if (blocking) sigaction(SIGCHLD, &old, NULL);
}

void runautostart(void) {
  static const char blocking_default[] =
      "#!/bin/sh\n# Optional blocking startup commands.\n";
  static const char autostart_default[] =
      "#!/bin/sh\n"
      "# Generated once by dwm. Edit this file to customise startup.\n"
      "if command -v nm-applet >/dev/null 2>&1; then nm-applet & fi\n"
      "if command -v xrandr >/dev/null 2>&1 && "
      "xrandr --query 2>/dev/null | grep -q '^eDP-1 connected' && "
      "xrandr --query 2>/dev/null | grep -q '^HDMI-1 connected'; then\n"
      "    xrandr --output eDP-1 --right-of HDMI-1\nfi\n"
      "if [ -n \"${DWM_AUTOSTART_BIN:-}\" ] && [ -x \"$DWM_AUTOSTART_BIN\" ]; then\n"
      "    \"$DWM_AUTOSTART_BIN\" --apply-display-settings\n"
      "elif command -v dwm >/dev/null 2>&1; then dwm --apply-display-settings; fi\n"
      "if [ -x \"$HOME/.fehbg\" ]; then\n    \"$HOME/.fehbg\" &\n"
      "elif [ -f \"$HOME/Downloads/wallpaper/glassmorphsesimjapon.png\" ] && "
      "command -v feh >/dev/null 2>&1; then\n"
      "    feh --bg-fill \"$HOME/Downloads/wallpaper/glassmorphsesimjapon.png\" &\nfi\n"
      "if command -v setxkbmap >/dev/null 2>&1; then "
      "setxkbmap -layout us,ir; fi\n";
  const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
  const char *wallpaper = getenv("DWM_WALLPAPERS_DIR");
  char xdgdir[PATH_MAX], legacy[PATH_MAX], path[PATH_MAX], configdir[PATH_MAX], configpath[PATH_MAX];
  char executable[PATH_MAX];
  const char *dir;
  struct stat st;
  ssize_t n;
  int length;
  if (!home || !*home) return;
  length = xdg && *xdg ? snprintf(xdgdir, sizeof xdgdir, "%s/dwm", xdg) :
                         snprintf(xdgdir, sizeof xdgdir, "%s/.local/share/dwm", home);
  if (length < 0 || (size_t)length >= sizeof xdgdir ||
      snprintf(legacy, sizeof legacy, "%s/.dwm", home) >= (int)sizeof legacy) return;
  if (stat(xdgdir, &st) == 0 && S_ISDIR(st.st_mode)) dir = xdgdir;
  else if (stat(legacy, &st) == 0 && S_ISDIR(st.st_mode)) dir = legacy;
  else dir = xdgdir;
  if (!mkdirparents(dir)) {
    fprintf(stderr, "dwm: cannot create autostart directory %s: %s\n", dir, strerror(errno));
    return;
  }
  if (!createifmissing(dir, autostartblocksh, blocking_default, 0755))
    fprintf(stderr, "dwm: cannot create blocking autostart: %s\n", strerror(errno));
  if (!createifmissing(dir, autostartsh, autostart_default, 0755))
    fprintf(stderr, "dwm: cannot create autostart: %s\n", strerror(errno));
  /* The mode file is intentionally empty until a user chooses a mode. */
  if (displayconfigpath(configdir, sizeof configdir, configpath, sizeof configpath)) {
    if (!mkdirparents(configdir) || !createifmissing(configdir, "refreshrate.conf", "", 0600))
      fprintf(stderr, "dwm: cannot initialise display settings: %s\n", strerror(errno));
  }
  if ((!wallpaper || !*wallpaper) &&
      snprintf(path, sizeof path, "%s/Downloads/wallpaper", home) < (int)sizeof path && !mkdirparents(path))
    fprintf(stderr, "dwm: cannot create wallpaper directory: %s\n", strerror(errno));
  /* The generated script can invoke this exact binary even if dwm is not on PATH. */
  n = readlink("/proc/self/exe", executable, sizeof executable - 1);
  if (n > 0 && (size_t)n < sizeof executable - 1) {
    executable[n] = '\0'; setenv("DWM_AUTOSTART_BIN", executable, 1);
  }
  if (snprintf(path, sizeof path, "%s/%s", dir, autostartblocksh) < (int)sizeof path)
    startautostart(path, 1);
  if (snprintf(path, sizeof path, "%s/%s", dir, autostartsh) < (int)sizeof path)
    startautostart(path, 0);
}

void scan(void) {
  unsigned int i, num;
  Window d1, d2, *wins = NULL;
  XWindowAttributes wa;

  if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
    for (i = 0; i < num; i++) {
      if (!XGetWindowAttributes(dpy, wins[i], &wa) || wa.override_redirect ||
          XGetTransientForHint(dpy, wins[i], &d1))
        continue;
      if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
        manage(wins[i], &wa);
    }
    for (i = 0; i < num; i++) { /* now the transients */
      if (!XGetWindowAttributes(dpy, wins[i], &wa))
        continue;
      if (XGetTransientForHint(dpy, wins[i], &d1) &&
          (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
        manage(wins[i], &wa);
    }
    if (wins)
      XFree(wins);
  }
}

void sendmon(Client *c, Monitor *m) {
  if (c->mon == m)
    return;
  unfocus(c, 1);
  detach(c);
  detachstack(c);
  c->mon = m;
  c->tags = m->tagset[m->seltags]; /* assign tags of target monitor */
  attach(c);
  attachstack(c);
  if (c->isfullscreen)
    resizeclient(c, m->mx, m->my, m->mw, m->mh);
  focus(NULL);
  arrange(NULL);
}

void setclientstate(Client *c, long state) {
  long data[] = {state, None};

  XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
                  PropModeReplace, (unsigned char *)data, 2);
}

int sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2,
              long d3, long d4) {
  int n;
  Atom *protocols, mt;
  int exists = 0;
  XEvent ev;

  if (proto == wmatom[WMTakeFocus] || proto == wmatom[WMDelete]) {
    mt = wmatom[WMProtocols];
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
      while (!exists && n--)
        exists = protocols[n] == proto;
      XFree(protocols);
    }
  } else {
    exists = True;
    mt = proto;
  }

  if (exists) {
    ev.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = mt;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = d0;
    ev.xclient.data.l[1] = d1;
    ev.xclient.data.l[2] = d2;
    ev.xclient.data.l[3] = d3;
    ev.xclient.data.l[4] = d4;
    XSendEvent(dpy, w, False, mask, &ev);
  }
  return exists;
}

void setfocus(Client *c) {
  if (!c->neverfocus)
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
  XChangeProperty(dpy, root, netatom[NetActiveWindow], XA_WINDOW, 32,
                  PropModeReplace, (unsigned char *)&c->win, 1);
  sendevent(c->win, wmatom[WMTakeFocus], NoEventMask, wmatom[WMTakeFocus],
            CurrentTime, 0, 0, 0);
}

void setfullscreen(Client *c, int fullscreen) {
  if (fullscreen && !c->isfullscreen) {
    XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&netatom[NetWMFullscreen],
                    1);
    c->isfullscreen = 1;
    c->oldstate = c->isfloating;
    c->oldbw = c->bw;
    c->bw = 0;
    c->isfloating = 1;
    resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
    XRaiseWindow(dpy, c->win);
  } else if (!fullscreen && c->isfullscreen) {
    XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)0, 0);
    c->isfullscreen = 0;
    c->isfloating = c->oldstate;
    c->bw = c->oldbw;
    c->x = c->oldx;
    c->y = c->oldy;
    c->w = c->oldw;
    c->h = c->oldh;
    resizeclient(c, c->x, c->y, c->w, c->h);
    arrange(c->mon);
  }
}

void setgaps(const Arg *arg) {
  switch (arg->i) {
  case GAP_TOGGLE:
    selmon->drawwithgaps = !selmon->drawwithgaps;
    break;
  case GAP_RESET:
    selmon->gappx = gappx;
    break;
  default:
    if (selmon->gappx + arg->i < 0)
      selmon->gappx = 0;
    else
      selmon->gappx += arg->i;
  }
  arrange(selmon);
}

void setlayout(const Arg *arg) {
  if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
    selmon->sellt ^= 1;
  if (arg && arg->v)
    selmon->lt[selmon->sellt] = (Layout *)arg->v;
  strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol,
          sizeof selmon->ltsymbol);
  if (selmon->sel)
    arrange(selmon);
  else
    drawbar(selmon);
}

/* arg > 1.0 will set mfact absolutely */
void setmfact(const Arg *arg) {
  float f;

  if (!arg || !selmon->lt[selmon->sellt]->arrange)
    return;
  f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
  if (f < 0.05 || f > 0.95)
    return;
  selmon->mfact = f;
  arrange(selmon);
}

void setup(void) {
  int i;
  XSetWindowAttributes wa;
  Atom utf8string;
  struct sigaction sa;

  /* do not transform children into zombies when they terminate */
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
  sa.sa_handler = SIG_IGN;
  sigaction(SIGCHLD, &sa, NULL);

  /* clean up any zombies (inherited from .xinitrc etc) immediately */
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;

  /* init screen */
  screen = DefaultScreen(dpy);
  sw = DisplayWidth(dpy, screen);
  sh = DisplayHeight(dpy, screen);
  root = RootWindow(dpy, screen);
  drw = drw_create(dpy, screen, root, sw, sh);
  if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
    die("no fonts could be loaded.");
  lrpad = drw->fonts->h;
  bh = drw->fonts->h + 2;
#ifdef DWM_CUI
  bh = MAX(bh, 36);
#endif
  updategeom();
  /* init atoms */
  xkbrules = XInternAtom(dpy, "_XKB_RULES_NAMES", False);
  utf8string = XInternAtom(dpy, "UTF8_STRING", False);
  wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
  wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
  wmatom[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
  netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
  netatom[NetSystemTray] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
  netatom[NetSystemTrayOP] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
  netatom[NetSystemTrayOrientation] =
      XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
  netatom[NetSystemTrayOrientationHorz] =
      XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION_HORZ", False);
  netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
  netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
  netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
  netatom[NetWMFullscreen] =
      XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
  netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  netatom[NetWMWindowTypeDialog] =
      XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
  xatom[Manager] = XInternAtom(dpy, "MANAGER", False);
  xatom[Xembed] = XInternAtom(dpy, "_XEMBED", False);
  xatom[XembedInfo] = XInternAtom(dpy, "_XEMBED_INFO", False);
  /* init cursors */
  cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
  cursor[CurResize] = drw_cur_create(drw, XC_sizing);
  cursor[CurMove] = drw_cur_create(drw, XC_fleur);
  /* init appearance */
  scheme = ecalloc(LENGTH(colors) + 1, sizeof(Clr *));
  scheme[LENGTH(colors)] = drw_scm_create(drw, colors[0], 3);
  for (i = 0; i < LENGTH(colors); i++)
    scheme[i] = drw_scm_create(drw, colors[i], 3);
  /* init system tray */
  updatesystray();
  /* init bars */
  updatebars();
  updatestatus();
  /* supporting window for NetWMCheck */
  wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
  XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
                  PropModeReplace, (unsigned char *)&wmcheckwin, 1);
  XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
                  PropModeReplace, (unsigned char *)"dwm", 3);
  XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
                  PropModeReplace, (unsigned char *)&wmcheckwin, 1);
  /* EWMH support per view */
  XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
                  PropModeReplace, (unsigned char *)netatom, NetLast);
  XDeleteProperty(dpy, root, netatom[NetClientList]);
  /* select events */
  wa.cursor = cursor[CurNormal]->cursor;
  wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                  ButtonPressMask | PointerMotionMask | EnterWindowMask |
                  LeaveWindowMask | StructureNotifyMask | PropertyChangeMask;
  XChangeWindowAttributes(dpy, root, CWEventMask | CWCursor, &wa);
  XSelectInput(dpy, root, wa.event_mask);
  grabkeys();
  focus(NULL);
}

void seturgent(Client *c, int urg) {
  XWMHints *wmh;

  c->isurgent = urg;
  if (!(wmh = XGetWMHints(dpy, c->win)))
    return;
  wmh->flags = urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
  XSetWMHints(dpy, c->win, wmh);
  XFree(wmh);
}

void showhide(Client *c) {
  if (!c)
    return;
  if (ISVISIBLE(c)) {
    /* show clients top down */
    XMoveWindow(dpy, c->win, c->x, c->y);
    if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating) &&
        !c->isfullscreen)
      resize(c, c->x, c->y, c->w, c->h, 0);
    showhide(c->snext);
  } else {
    /* hide clients bottom up */
    showhide(c->snext);
    XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
  }
}

void spawn(const Arg *arg) {
  struct sigaction sa;

  if (arg->v == dmenucmd)
    dmenumon[0] = '0' + selmon->num;
  if (fork() == 0) {
    if (dpy)
      close(ConnectionNumber(dpy));
    setsid();

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    execvp(((char **)arg->v)[0], (char **)arg->v);
    die("dwm: execvp '%s' failed:", ((char **)arg->v)[0]);
  }
}

void tag(const Arg *arg) {
  if (selmon->sel && arg->ui & TAGMASK) {
    selmon->sel->tags = arg->ui & TAGMASK;
    focus(NULL);
    arrange(selmon);
  }
}

void tagmon(const Arg *arg) {
  if (!selmon->sel || !mons->next)
    return;
  sendmon(selmon->sel, dirtomon(arg->i));
}

void tile(Monitor *m) {
  unsigned int i, n, h, mw, my, ty;
  Client *c;

  for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++)
    ;
  if (n == 0)
    return;
  if (m->drawwithgaps) { /* draw with fullgaps logic */
    if (n > m->nmaster)
      mw = m->nmaster ? m->ww * m->mfact : 0;
    else
      mw = m->ww - m->gappx;
    for (i = 0, my = ty = m->gappx, c = nexttiled(m->clients); c;
         c = nexttiled(c->next), i++)
      if (i < m->nmaster) {
        h = (m->wh - my) / (MIN(n, m->nmaster) - i) - m->gappx;
        resize(c, m->wx + m->gappx, m->wy + my, mw - (2 * c->bw) - m->gappx,
               h - (2 * c->bw), 0);
        if (my + HEIGHT(c) + m->gappx < m->wh)
          my += HEIGHT(c) + m->gappx;
      } else {
        h = (m->wh - ty) / (n - i) - m->gappx;
        resize(c, m->wx + mw + m->gappx, m->wy + ty,
               m->ww - mw - (2 * c->bw) - 2 * m->gappx, h - (2 * c->bw), 0);
        if (ty + HEIGHT(c) + m->gappx < m->wh)
          ty += HEIGHT(c) + m->gappx;
      }
  } else { /* draw with singularborders logic */
    if (n > m->nmaster)
      mw = m->nmaster ? m->ww * m->mfact : 0;
    else
      mw = m->ww;
    for (i = my = ty = 0, c = nexttiled(m->clients); c;
         c = nexttiled(c->next), i++)
      if (i < m->nmaster) {
        h = (m->wh - my) / (MIN(n, m->nmaster) - i);
        if (n == 1)
          resize(c, m->wx - c->bw, m->wy, m->ww, m->wh, False);
        else
          resize(c, m->wx - c->bw, m->wy + my, mw - c->bw, h - c->bw, False);
        my += HEIGHT(c) - c->bw;
      } else {
        h = (m->wh - ty) / (n - i);
        resize(c, m->wx + mw - c->bw, m->wy + ty, m->ww - mw, h - c->bw, False);
        ty += HEIGHT(c) - c->bw;
      }
  }
}

void togglebar(const Arg *arg) {
  selmon->showbar = !selmon->showbar;
  updatebarpos(selmon);
  resizebarwin(selmon);
  if (showsystray && systray && selmon == systraytomon(selmon)) {
    XWindowChanges wc;
    wc.y = selmon->by;
    XConfigureWindow(dpy, systray->win, CWY, &wc);
  }
  arrange(selmon);
}

void togglefloating(const Arg *arg) {
  if (!selmon->sel)
    return;
  if (selmon->sel->isfullscreen) /* no support for fullscreen windows */
    return;
  selmon->sel->isfloating = !selmon->sel->isfloating || selmon->sel->isfixed;
  if (selmon->sel->isfloating)
    resize(selmon->sel, selmon->sel->x, selmon->sel->y, selmon->sel->w,
           selmon->sel->h, 0);
  arrange(selmon);
}

void toggletag(const Arg *arg) {
  unsigned int newtags;

  if (!selmon->sel)
    return;
  newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
  if (newtags) {
    selmon->sel->tags = newtags;
    focus(NULL);
    arrange(selmon);
  }
}

void toggleview(const Arg *arg) {
  unsigned int newtagset =
      selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);

  if (newtagset) {
    selmon->tagset[selmon->seltags] = newtagset;
    focus(NULL);
    arrange(selmon);
  }
}

void unfocus(Client *c, int setfocus) {
  if (!c)
    return;
  grabbuttons(c, 0);
  XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
  if (setfocus) {
    XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
  }
}

void unmanage(Client *c, int destroyed) {
  Monitor *m = c->mon;
  XWindowChanges wc;

  detach(c);
  detachstack(c);
  if (!destroyed) {
    wc.border_width = c->oldbw;
    XGrabServer(dpy); /* avoid race conditions */
    XSetErrorHandler(xerrordummy);
    XSelectInput(dpy, c->win, NoEventMask);
    XConfigureWindow(dpy, c->win, CWBorderWidth, &wc); /* restore border */
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    setclientstate(c, WithdrawnState);
    XSync(dpy, False);
    XSetErrorHandler(xerror);
    XUngrabServer(dpy);
  }
  free(c);
  focus(NULL);
  updateclientlist();
  arrange(m);
}

void unmapnotify(XEvent *e) {
  Client *c;
  XUnmapEvent *ev = &e->xunmap;

  if ((c = wintoclient(ev->window))) {
    if (ev->send_event)
      setclientstate(c, WithdrawnState);
    else
      unmanage(c, 0);
  } else if ((c = wintosystrayicon(ev->window))) {
    /* KLUDGE! sometimes icons occasionally unmap their windows, but do
     * _not_ destroy them. We map those windows back */
    XMapRaised(dpy, c->win);
    updatesystray();
  }
}

void updatebars(void) {
  unsigned int w;
  Monitor *m;
  XSetWindowAttributes wa = {.override_redirect = True,
                             .background_pixmap = ParentRelative,
                             .event_mask = ButtonPressMask | ExposureMask};
  XClassHint ch = {"dwm", "dwm"};
  for (m = mons; m; m = m->next) {
    if (m->barwin)
      continue;
    w = m->ww;
    if (showsystray && m == systraytomon(m))
      w -= getsystraywidth();
    m->barwin = XCreateWindow(
        dpy, root, m->wx, m->by, w, bh, 0, DefaultDepth(dpy, screen),
        CopyFromParent, DefaultVisual(dpy, screen),
        CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
    XDefineCursor(dpy, m->barwin, cursor[CurNormal]->cursor);
    if (showsystray && systray && m == systraytomon(m))
      XMapRaised(dpy, systray->win);
    XMapRaised(dpy, m->barwin);
    XSetClassHint(dpy, m->barwin, &ch);
  }
}

void updatebarpos(Monitor *m) {
  m->wy = m->my;
  m->wh = m->mh;
  if (m->showbar) {
    m->wh -= bh;
    m->by = m->topbar ? m->wy : m->wy + m->wh;
    m->wy = m->topbar ? m->wy + bh : m->wy;
  } else
    m->by = -bh;
}

void updateclientlist(void) {
  Client *c;
  Monitor *m;

  XDeleteProperty(dpy, root, netatom[NetClientList]);
  for (m = mons; m; m = m->next)
    for (c = m->clients; c; c = c->next)
      XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
                      PropModeAppend, (unsigned char *)&(c->win), 1);
}

int updategeom(void) {
  int dirty = 0;

#ifdef XINERAMA
  if (XineramaIsActive(dpy)) {
    int i, j, n, nn;
    Client *c;
    Monitor *m;
    XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nn);
    XineramaScreenInfo *unique = NULL;

    for (n = 0, m = mons; m; m = m->next, n++)
      ;
    /* only consider unique geometries as separate screens */
    unique = ecalloc(nn, sizeof(XineramaScreenInfo));
    for (i = 0, j = 0; i < nn; i++)
      if (isuniquegeom(unique, j, &info[i]))
        memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
    XFree(info);
    nn = j;

    /* new monitors if nn > n */
    for (i = n; i < nn; i++) {
      for (m = mons; m && m->next; m = m->next)
        ;
      if (m)
        m->next = createmon();
      else
        mons = createmon();
    }
    for (i = 0, m = mons; i < nn && m; m = m->next, i++)
      if (i >= n || unique[i].x_org != m->mx || unique[i].y_org != m->my ||
          unique[i].width != m->mw || unique[i].height != m->mh) {
        dirty = 1;
        m->num = i;
        m->mx = m->wx = unique[i].x_org;
        m->my = m->wy = unique[i].y_org;
        m->mw = m->ww = unique[i].width;
        m->mh = m->wh = unique[i].height;
        updatebarpos(m);
      }
    /* removed monitors if n > nn */
    for (i = nn; i < n; i++) {
      for (m = mons; m && m->next; m = m->next)
        ;
      while ((c = m->clients)) {
        dirty = 1;
        m->clients = c->next;
        detachstack(c);
        c->mon = mons;
        attach(c);
        attachstack(c);
      }
      if (m == selmon)
        selmon = mons;
      cleanupmon(m);
    }
    free(unique);
  } else
#endif /* XINERAMA */
  {    /* default monitor setup */
    if (!mons)
      mons = createmon();
    if (mons->mw != sw || mons->mh != sh) {
      dirty = 1;
      mons->mw = mons->ww = sw;
      mons->mh = mons->wh = sh;
      updatebarpos(mons);
    }
  }
  if (dirty) {
    selmon = mons;
    selmon = wintomon(root);
  }
  return dirty;
}

void updatenumlockmask(void) {
  unsigned int i, j;
  XModifierKeymap *modmap;

  numlockmask = 0;
  modmap = XGetModifierMapping(dpy);
  for (i = 0; i < 8; i++)
    for (j = 0; j < modmap->max_keypermod; j++)
      if (modmap->modifiermap[i * modmap->max_keypermod + j] ==
          XKeysymToKeycode(dpy, XK_Num_Lock))
        numlockmask = (1 << i);
  XFreeModifiermap(modmap);
}

void updatesizehints(Client *c) {
  long msize;
  XSizeHints size;

  if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
    /* size is uninitialized, ensure that size.flags aren't used */
    size.flags = PSize;
  if (size.flags & PBaseSize) {
    c->basew = size.base_width;
    c->baseh = size.base_height;
  } else if (size.flags & PMinSize) {
    c->basew = size.min_width;
    c->baseh = size.min_height;
  } else
    c->basew = c->baseh = 0;
  if (size.flags & PResizeInc) {
    c->incw = size.width_inc;
    c->inch = size.height_inc;
  } else
    c->incw = c->inch = 0;
  if (size.flags & PMaxSize) {
    c->maxw = size.max_width;
    c->maxh = size.max_height;
  } else
    c->maxw = c->maxh = 0;
  if (size.flags & PMinSize) {
    c->minw = size.min_width;
    c->minh = size.min_height;
  } else if (size.flags & PBaseSize) {
    c->minw = size.base_width;
    c->minh = size.base_height;
  } else
    c->minw = c->minh = 0;
  if (size.flags & PAspect) {
    c->mina = (float)size.min_aspect.y / size.min_aspect.x;
    c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
  } else
    c->maxa = c->mina = 0.0;
  c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
  c->hintsvalid = 1;
}

void updatestatus(void) {
  if (!gettextprop(root, XA_WM_NAME, stext, sizeof(stext)))
    strcpy(stext, "dwm-" VERSION);
  drawbar(selmon);
  updatesystray();
}

void updatesystrayicongeom(Client *i, int w, int h) {
  if (i) {
    i->h = bh;
    if (w == h)
      i->w = bh;
    else if (h == bh)
      i->w = w;
    else
      i->w = (int)((float)bh * ((float)w / (float)h));
    applysizehints(i, &(i->x), &(i->y), &(i->w), &(i->h), False);
    /* force icons into the systray dimensions if they don't want to */
    if (i->h > bh) {
      if (i->w == i->h)
        i->w = bh;
      else
        i->w = (int)((float)bh * ((float)i->w / (float)i->h));
      i->h = bh;
    }
  }
}

void updatesystrayiconstate(Client *i, XPropertyEvent *ev) {
  long flags;
  int code = 0;

  if (!showsystray || !i || ev->atom != xatom[XembedInfo] ||
      !(flags = getatomprop(i, xatom[XembedInfo])))
    return;

  if (flags & XEMBED_MAPPED && !i->tags) {
    i->tags = 1;
    code = XEMBED_WINDOW_ACTIVATE;
    XMapRaised(dpy, i->win);
    setclientstate(i, NormalState);
  } else if (!(flags & XEMBED_MAPPED) && i->tags) {
    i->tags = 0;
    code = XEMBED_WINDOW_DEACTIVATE;
    XUnmapWindow(dpy, i->win);
    setclientstate(i, WithdrawnState);
  } else
    return;
  sendevent(i->win, xatom[Xembed], StructureNotifyMask, CurrentTime, code, 0,
            systray->win, XEMBED_EMBEDDED_VERSION);
}

void updatesystray(void) {
  XSetWindowAttributes wa;
  XWindowChanges wc;
  Client *i;
  Monitor *m = systraytomon(NULL);
  unsigned int x = m->mx + m->mw;
  unsigned int sw = TEXTW(stext) - lrpad + systrayspacing;
  unsigned int w = 1;

  if (!showsystray)
    return;
  if (systrayonleft)
    x -= sw + lrpad / 2;
  if (!systray) {
    /* init systray */
    if (!(systray = (Systray *)calloc(1, sizeof(Systray))))
      die("fatal: could not malloc() %u bytes\n", sizeof(Systray));
    systray->win = XCreateSimpleWindow(dpy, root, x, m->by, w, bh, 0, 0,
                                       scheme[SchemeSel][ColBg].pixel);
    wa.event_mask = ButtonPressMask | ExposureMask;
    wa.override_redirect = True;
    wa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
    XSelectInput(dpy, systray->win, SubstructureNotifyMask);
    XChangeProperty(dpy, systray->win, netatom[NetSystemTrayOrientation],
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&netatom[NetSystemTrayOrientationHorz], 1);
    XChangeWindowAttributes(
        dpy, systray->win, CWEventMask | CWOverrideRedirect | CWBackPixel, &wa);
    XMapRaised(dpy, systray->win);
    XSetSelectionOwner(dpy, netatom[NetSystemTray], systray->win, CurrentTime);
    if (XGetSelectionOwner(dpy, netatom[NetSystemTray]) == systray->win) {
      sendevent(root, xatom[Manager], StructureNotifyMask, CurrentTime,
                netatom[NetSystemTray], systray->win, 0, 0);
      XSync(dpy, False);
    } else {
      fprintf(stderr, "dwm: unable to obtain system tray.\n");
      free(systray);
      systray = NULL;
      return;
    }
  }
  for (w = 0, i = systray->icons; i; i = i->next) {
    /* make sure the background color stays the same */
    wa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
    XChangeWindowAttributes(dpy, i->win, CWBackPixel, &wa);
    XMapRaised(dpy, i->win);
    w += systrayspacing;
    i->x = w;
    XMoveResizeWindow(dpy, i->win, i->x, 0, i->w, i->h);
    w += i->w;
    if (i->mon != m)
      i->mon = m;
  }
  w = w ? w + systrayspacing : 1;
  x -= w;
  XMoveResizeWindow(dpy, systray->win, x, m->by, w, bh);
  wc.x = x;
  wc.y = m->by;
  wc.width = w;
  wc.height = bh;
  wc.stack_mode = Above;
  wc.sibling = m->barwin;
  XConfigureWindow(dpy, systray->win,
                   CWX | CWY | CWWidth | CWHeight | CWSibling | CWStackMode,
                   &wc);
  XMapWindow(dpy, systray->win);
  XMapSubwindows(dpy, systray->win);
  /* redraw background */
  XSetForeground(dpy, drw->gc, scheme[SchemeNorm][ColBg].pixel);
  XFillRectangle(dpy, systray->win, drw->gc, 0, 0, w, bh);
  XSync(dpy, False);
}

void updatetitle(Client *c) {
  if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
    gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
  if (c->name[0] == '\0') /* hack to mark broken clients */
    strcpy(c->name, broken);
}

void updatewindowtype(Client *c) {
  Atom state = getatomprop(c, netatom[NetWMState]);
  Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

  if (state == netatom[NetWMFullscreen])
    setfullscreen(c, 1);
  if (wtype == netatom[NetWMWindowTypeDialog])
    c->isfloating = 1;
}

void updatewmhints(Client *c) {
  XWMHints *wmh;

  if ((wmh = XGetWMHints(dpy, c->win))) {
    if (c == selmon->sel && wmh->flags & XUrgencyHint) {
      wmh->flags &= ~XUrgencyHint;
      XSetWMHints(dpy, c->win, wmh);
    } else
      c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
    if (wmh->flags & InputHint)
      c->neverfocus = !wmh->input;
    else
      c->neverfocus = 0;
    XFree(wmh);
  }
}

void view(const Arg *arg) {
  if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
    return;
  selmon->seltags ^= 1; /* toggle sel tagset */
  if (arg->ui & TAGMASK)
    selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
  focus(NULL);
  arrange(selmon);
}

Client *wintoclient(Window w) {
  Client *c;
  Monitor *m;

  for (m = mons; m; m = m->next)
    for (c = m->clients; c; c = c->next)
      if (c->win == w)
        return c;
  return NULL;
}

Client *wintosystrayicon(Window w) {
  Client *i = NULL;

  if (!showsystray || !systray || !w)
    return i;
  for (i = systray->icons; i && i->win != w; i = i->next)
    ;
  return i;
}

Monitor *wintomon(Window w) {
  int x, y;
  Client *c;
  Monitor *m;

  if (w == root && getrootptr(&x, &y))
    return recttomon(x, y, 1, 1);
  for (m = mons; m; m = m->next)
    if (w == m->barwin)
      return m;
  if ((c = wintoclient(w)))
    return c->mon;
  return selmon;
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's). Other types of errors call Xlibs
 * default error handler, which may call exit. */
int xerror(Display *dpy, XErrorEvent *ee) {
  if (ee->error_code == BadWindow ||
      (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch) ||
      (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable) ||
      (ee->request_code == X_PolyFillRectangle &&
       ee->error_code == BadDrawable) ||
      (ee->request_code == X_PolySegment && ee->error_code == BadDrawable) ||
      (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch) ||
      (ee->request_code == X_GrabButton && ee->error_code == BadAccess) ||
      (ee->request_code == X_GrabKey && ee->error_code == BadAccess) ||
      (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
    return 0;
  fprintf(stderr, "dwm: fatal error: request code=%d, error code=%d\n",
          ee->request_code, ee->error_code);
  return xerrorxlib(dpy, ee); /* may call exit */
}

int xerrordummy(Display *dpy, XErrorEvent *ee) { return 0; }

/* Startup Error handler to check if another window manager
 * is already running. */
int xerrorstart(Display *dpy, XErrorEvent *ee) {
  die("dwm: another window manager is already running");
  return -1;
}

Monitor *systraytomon(Monitor *m) {
  Monitor *t;
  int i, n;
  if (!systraypinning) {
    if (!m)
      return selmon;
    return m == selmon ? m : NULL;
  }
  for (n = 1, t = mons; t && t->next; n++, t = t->next)
    ;
  for (i = 1, t = mons; t && t->next && i < systraypinning; i++, t = t->next)
    ;
  if (systraypinningfailfirst && n < systraypinning)
    return mons;
  return t;
}

void zoom(const Arg *arg) {
  Client *c = selmon->sel;

  if (!selmon->lt[selmon->sellt]->arrange || !c || c->isfloating)
    return;
  if (c == nexttiled(selmon->clients) && !(c = nexttiled(c->next)))
    return;
  pop(c);
}
void warp(const Client *c) {
  int x, y;

  if (!c) {
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, selmon->wx + selmon->ww / 2,
                 selmon->wy + selmon->wh / 2);
    return;
  }

  if (!getrootptr(&x, &y) ||
      (x > c->x - c->bw && y > c->y - c->bw && x < c->x + c->w + c->bw * 2 &&
       y < c->y + c->h + c->bw * 2) ||
      (y > c->mon->by && y < c->mon->by + bh) || (c->mon->topbar && !y))
    return;

  XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w / 2, c->h / 2);
}

int main(int argc, char *argv[]) {
  if (argc == 2 && !strcmp("--apply-display-settings", argv[1]))
    return restoredisplaymodes();
  if (argc == 2 && !strcmp("-v", argv[1]))
    die("dwm-" VERSION);
  else if (argc != 1)
    die("usage: dwm [-v | --apply-display-settings]");
  if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
    fputs("warning: no locale support\n", stderr);
  if (!(dpy = XOpenDisplay(NULL)))
    die("dwm: cannot open display");
  checkotherwm();
  setup();
#ifdef __OpenBSD__
  if (pledge("stdio rpath proc exec", NULL) == -1)
    die("pledge");
#endif /* __OpenBSD__ */
  scan();
  runautostart();
  run();
  cleanup();
  XCloseDisplay(dpy);
  return EXIT_SUCCESS;
}
