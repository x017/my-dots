/* Headless XKB and locker-launch regression tests. Xlib calls are mocked;
 * setxkbmap and mel are harmless temporary scripts, not the real programs. */
#define _POSIX_C_SOURCE 200809L
#include "../lock_screen.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>

static const unsigned char with_toggle[] = "evdev\0pc105\0ir,us\0\0compose:ralt,grp:win_space_toggle";
static const unsigned char without_toggle[] = "evdev\0pc105\0us,ir\0\0caps:escape,compose:ralt";
static const unsigned char no_english[] = "evdev\0pc105\0ir,de\0\0";
static const unsigned char brit_first[] = "evdev\0pc105\0gb,ir\0\0";
static const unsigned char substring[] = "evdev\0pc105\0us,ir\0\0grp:win_space_toggle_extra";
static const unsigned char *property = with_toggle;
static size_t property_size = sizeof with_toggle;
static int locked_group = -1, sync_count;
static int state_available = 1;

Atom XInternAtom(Display *d, const char *name, Bool only) {
    (void)d; (void)only; assert(!strcmp(name, "_XKB_RULES_NAMES")); return 42;
}
int XGetWindowProperty(Display *d, Window w, Atom prop, long offset, long len,
                       Bool del, Atom requested, Atom *actual, int *format,
                       unsigned long *count, unsigned long *remaining, unsigned char **data) {
    (void)d; (void)w; (void)prop; (void)offset; (void)len; (void)del; (void)requested;
    *actual = property ? XA_STRING : None; *format = 8; *count = property_size; *remaining = 0;
    *data = (unsigned char *)property; return Success;
}
int XFree(void *p) { (void)p; return 0; }
Status XkbGetState(Display *d, unsigned int dev, XkbStatePtr state) {
    (void)d; (void)dev; memset(state, 0, sizeof *state); state->locked_group = 1;
    return state_available ? Success : 1;
}
Bool XkbLockGroup(Display *d, unsigned int dev, unsigned int group) {
    (void)d; (void)dev; locked_group = (int)group; return True;
}
int XSync(Display *d, Bool discard) { (void)d; (void)discard; sync_count++; return 0; }
Display *XOpenDisplay(const char *name) { (void)name; return NULL; }
int XCloseDisplay(Display *d) { (void)d; return 0; }

static void script(const char *path, const char *body) {
    FILE *f = fopen(path, "w"); assert(f); assert(fputs(body, f) >= 0); assert(fclose(f) == 0); assert(chmod(path, 0700) == 0);
}
static int exists(const char *path) { return access(path, F_OK) == 0; }

int main(void) {
    char dir[] = "/tmp/dwm-lock-test-XXXXXX", helper[256], log[256], locker[256], text[256];
    Display *fake = (Display *)1;
    int status; FILE *f;
    const char *const cmd[] = {"mel", "--test", NULL};
    assert(mkdtemp(dir));
    snprintf(helper, sizeof helper, "%s/setxkbmap", dir);
    snprintf(locker, sizeof locker, "%s/mel", dir);
    snprintf(log, sizeof log, "%s/log", dir);
    assert(setenv("PATH", dir, 1) == 0);
    assert(setenv("LOCK_TEST_LOG", log, 1) == 0);
    script(helper, "#!/bin/sh\nprintf '%s\\n' \"$*\" > \"$LOCK_TEST_LOG\"\n");
    script(locker, "#!/bin/sh\nprintf 'mel %s\\n' \"$*\" > \"$LOCK_TEST_LOG\"\n");

    assert(lock_xkb_option(with_toggle, sizeof with_toggle, "grp:win_space_toggle"));
    assert(lock_xkb_option(with_toggle, sizeof with_toggle - 1, "grp:win_space_toggle"));
    assert(!lock_xkb_option(without_toggle, sizeof without_toggle, "grp:win_space_toggle"));
    assert(!lock_xkb_option(substring, sizeof substring, "grp:win_space_toggle"));
    assert(!lock_xkb_option(with_toggle, 5, "grp:win_space_toggle"));
    assert(lock_xkb_english_group(with_toggle, sizeof with_toggle) == 1);
    assert(lock_xkb_english_group(without_toggle, sizeof without_toggle) == 0);
    assert(lock_xkb_english_group(brit_first, sizeof brit_first) == 0);
    assert(lock_xkb_english_group(no_english, sizeof no_english) == -1);
    assert(lock_xkb_english_group(with_toggle, 7) == -1);

    /* Existing XKB option is not installed twice; English group can be second. */
    lock_prepare_keyboard(fake, 1);
    assert(locked_group == 1 && sync_count == 1 && !exists(log));

    /* Append only the option; never reset other options or set a new layout. */
    property = without_toggle; property_size = sizeof without_toggle; locked_group = -1;
    lock_prepare_keyboard(fake, 1);
    assert(locked_group == 0 && exists(log));
    f = fopen(log, "r"); assert(f && fgets(text, sizeof text, f)); fclose(f);
    assert(!strcmp(text, "-option grp:win_space_toggle\n"));
    unlink(log);

    /* If there is no English layout, retain the locked group rather than
     * assuming group zero is English. */
    property = no_english; property_size = sizeof no_english; locked_group = -1;
    lock_prepare_keyboard(fake, 1); assert(locked_group == 1);
    unlink(log);

    /* Unknown or truncated custom maps must not be overwritten by setxkbmap. */
    property = NULL; property_size = 0; locked_group = -1;
    lock_prepare_keyboard(fake, 1); assert(!exists(log) && locked_group == 1);
    property = without_toggle; property_size = 5; locked_group = -1;
    lock_prepare_keyboard(fake, 1); assert(!exists(log) && locked_group == 1);
    state_available = 0; locked_group = -1;
    lock_prepare_keyboard(fake, 1); assert(locked_group == -1);
    state_available = 1;

    /* A stuck helper is killed after about one second instead of blocking the
     * locker forever. */
    {
        struct timespec before, after;
        double elapsed;
        script(helper, "#!/bin/sh\nexec /bin/sleep 5\n");
        assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
        assert(!lock_enable_xkb_shortcut());
        assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
        elapsed = after.tv_sec - before.tv_sec + (after.tv_nsec - before.tv_nsec) / 1e9;
        assert(elapsed < 3.0);
    }
    /* Missing setxkbmap must not prevent the English fallback or locking. */
    unlink(helper); property = without_toggle; property_size = sizeof without_toggle;
    locked_group = -1; lock_prepare_keyboard(fake, 1); assert(locked_group == 0);
    lock_screen_start(NULL, cmd);
    assert(waitpid(-1, &status, 0) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    f = fopen(log, "r"); assert(f && fgets(text, sizeof text, f)); fclose(f);
    assert(!strcmp(text, "mel --test\n"));
    unlink(log); unlink(locker); rmdir(dir);
    puts("lock XKB parsing, fallback, option preservation and locker launch passed");
    return 0;
}
