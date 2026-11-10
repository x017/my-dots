/* Exercises screenshot_dir, the real directory logic used by the capture
 * child, with a fake HOME. The unused X11 helpers are dropped by --gc-sections. */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

/* Minimal dwm surface that screenshot.h's event helpers reference. */
typedef struct { int i; unsigned int ui; float f; const void *v; } Arg;
#define LENGTH(x) ((int)(sizeof (x) / sizeof (x)[0]))
typedef struct Monitor Monitor;
struct Monitor { int num, mx, my, mw, mh; Monitor *next; };
static Display *dpy; static int screen; static Window root; static int running = 1;
static int sw, sh; static Monitor *mons, *selmon;
static void configurenotify(XEvent *e) { (void)e; }
static int displaypick(const char *title, char **items, int count)
{ (void)title; (void)items; return count > 0 ? 0 : -1; }
static void (*handler[LASTEvent])(XEvent *);
/* The two key handlers need dwm globals this test does not provide; they are
 * compiled but discarded by --gc-sections, so silence the unused warnings. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../screenshot.h"
#pragma GCC diagnostic pop

static void clear_var(const char *k) { unsetenv(k); }

int main(void) {
  char dir[PATH_MAX], home[] = "/tmp/dwm-shot-home-XXXXXX", want[PATH_MAX];
  struct stat st;
  assert(mkdtemp(home));
  assert(setenv("HOME", home, 1) == 0);
  clear_var("DWM_SCREENSHOT_DIR");
  /* Default location is created on demand under $HOME/Pictures/Screenshots. */
  assert(screenshot_dir(dir, sizeof dir));
  assert(snprintf(want, sizeof want, "%s/Pictures/Screenshots", home) > 0);
  assert(!strcmp(dir, want) && !stat(dir, &st) && S_ISDIR(st.st_mode));
  /* An absolute override wins and is created if missing. */
  snprintf(want, sizeof want, "%s/custom", home);
  assert(setenv("DWM_SCREENSHOT_DIR", want, 1) == 0);
  assert(screenshot_dir(dir, sizeof dir) && !strcmp(dir, want) && !stat(dir, &st));
  /* A relative path must be refused rather than written to an unknown place. */
  assert(setenv("DWM_SCREENSHOT_DIR", "relative/shot", 1) == 0);
  assert(!screenshot_dir(dir, sizeof dir));
  /* No usable HOME means no safe destination. */
  clear_var("DWM_SCREENSHOT_DIR"); clear_var("HOME");
  assert(!screenshot_dir(dir, sizeof dir));
  snprintf(want, sizeof want, "rm -rf %s", home); assert(system(want) == 0);
  puts("screenshot destination directory selection passed");
  return 0;
}
