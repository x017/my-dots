/* Where screenshots are written. Kept separate so it can be exercised without
 * an X server. */
#ifndef DWM_SCREENSHOT_DIR_H
#define DWM_SCREENSHOT_DIR_H
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int screenshot_dir(char *out, size_t size) {
  const char *override = getenv("DWM_SCREENSHOT_DIR"), *home = getenv("HOME");
  char parent[PATH_MAX]; struct stat st; int n;
  if (override && *override) {
    /* A relative path would land somewhere the session never looks. */
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
#endif
