/* Prepare the XKB keyboard before invoking the screen locker. The locker grabs
 * the keyboard, so shortcuts implemented by dwm cannot run while locked.
 * Keep layouts and existing XKB options; add the server-side Win+Space action
 * and start the locker on an English layout when one is configured. */
#ifndef DWM_LOCK_SCREEN_H
#define DWM_LOCK_SCREEN_H

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* _XKB_RULES_NAMES contains five consecutive NUL-terminated fields. */
static int lock_xkb_field(const unsigned char *data, size_t length, int field,
                          const unsigned char **start, size_t *size) {
    size_t i = 0, end;
    int n;
    if (!data || field < 0 || field >= 5) return 0;
    for (n = 0; n < field; n++) {
        while (i < length && data[i]) i++;
        if (i == length) return 0;
        i++;
    }
    if (i >= length) return 0;
    end = i;
    while (end < length && data[end]) end++;
    if (end == length && field != 4) return 0; /* required fields are terminated */
    *start = data + i; *size = end - i;
    return 1;
}

static int lock_xkb_option(const unsigned char *data, size_t length, const char *option) {
    const unsigned char *p; size_t len, pos = 0, end, want = strlen(option);
    if (!lock_xkb_field(data, length, 4, &p, &len)) return 0;
    while (pos < len) {
        end = pos;
        while (end < len && p[end] != ',') end++;
        if (end - pos == want && !memcmp(p + pos, option, want)) return 1;
        pos = end + 1;
    }
    return 0;
}

static int lock_xkb_english_group(const unsigned char *data, size_t length) {
    const unsigned char *p; size_t len, pos = 0, end;
    int group = 0;
    if (!lock_xkb_field(data, length, 2, &p, &len)) return -1;
    while (pos < len && group < 4) {
        end = pos;
        while (end < len && p[end] != ',') end++;
        if (end - pos == 2 && (!memcmp(p + pos, "us", 2) || !memcmp(p + pos, "gb", 2))) return group;
        pos = end + 1; group++;
    }
    return -1;
}

static unsigned char *lock_xkb_rules(Display *display, Window root, unsigned long *length) {
    Atom atom, actual; int format;
    unsigned long remaining;
    unsigned char *data = NULL;
    *length = 0;
    atom = XInternAtom(display, "_XKB_RULES_NAMES", False);
    if (XGetWindowProperty(display, root, atom, 0, 4096, False, XA_STRING,
                           &actual, &format, length, &remaining, &data) != Success ||
        actual != XA_STRING || format != 8 || remaining != 0) {
        if (data) XFree(data);
        *length = 0;
        return NULL;
    }
    return data;
}

/* Bound the helper wait: a broken setxkbmap must not indefinitely prevent
 * mel from being started. Never invoke a shell or clear other options. */
static int lock_enable_xkb_shortcut(void) {
    pid_t child = fork(), result;
    int status, i;
    struct timespec delay = {0, 20000000};
    if (child < 0) return 0;
    if (child == 0) {
        execlp("setxkbmap", "setxkbmap", "-option", "grp:win_space_toggle", (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 50; i++) {
        result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) return 0;
        nanosleep(&delay, NULL);
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) ;
    return 0;
}

static void lock_prepare_keyboard(Display *display, Window root) {
    unsigned char *rules;
    unsigned long length;
    int english, has_shortcut, valid_rules;
    const unsigned char *field;
    size_t field_length;
    XkbStateRec state;
    int have_state = XkbGetState(display, XkbUseCoreKbd, &state) == Success;

    rules = lock_xkb_rules(display, root, &length);
    has_shortcut = lock_xkb_option(rules, length, "grp:win_space_toggle");
    valid_rules = lock_xkb_field(rules, length, 0, &field, &field_length) && field_length &&
                  lock_xkb_field(rules, length, 2, &field, &field_length) && field_length &&
                  lock_xkb_field(rules, length, 4, &field, &field_length);
    /* Without trustworthy rules, setxkbmap could replace a custom xkbcomp
     * map with a default map. Leave such configurations untouched. */
    if (valid_rules && !has_shortcut && !lock_enable_xkb_shortcut())
        fprintf(stderr, "lock: unable to enable XKB Win+Space; starting locker anyway\n");
    if (rules) XFree(rules);
    /* setxkbmap may have changed the map and group. Read the installed map,
     * select its English group if available, otherwise retain the old group. */
    rules = lock_xkb_rules(display, root, &length);
    english = lock_xkb_english_group(rules, length);
    if (rules) XFree(rules);
    if (english >= 0) XkbLockGroup(display, XkbUseCoreKbd, (unsigned)english);
    else if (have_state) XkbLockGroup(display, XkbUseCoreKbd, state.locked_group);
    XSync(display, False);
}

/* Execute in the current process, so xss-lock can wait for mel to exit. */
static void lock_screen_exec(Display *parent_display, const char *const *command) {
    Display *display;
    if (parent_display) close(ConnectionNumber(parent_display));
    signal(SIGCHLD, SIG_DFL);
    display = XOpenDisplay(NULL);
    if (display) {
        lock_prepare_keyboard(display, DefaultRootWindow(display));
        XCloseDisplay(display);
    } else fprintf(stderr, "lock: cannot open X display; starting locker anyway\n");
    execvp(command[0], (char *const *)command);
    fprintf(stderr, "lock: cannot start %s: %s\n", command[0], strerror(errno));
    _exit(127);
}

/* The parent (dwm or bar) never waits and remains responsive. */
static void lock_screen_start(Display *parent_display, const char *const *command) {
    pid_t child = fork();
    if (child == 0) {
        setsid();
        lock_screen_exec(parent_display, command);
    }
    if (child < 0) fprintf(stderr, "lock: cannot start locker: %s\n", strerror(errno));
}
#endif
