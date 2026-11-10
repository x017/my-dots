#define _DEFAULT_SOURCE
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../screenshot_dir.h"


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
