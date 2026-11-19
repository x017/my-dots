/* Exercises the real layout and Cairo renderer without opening X or invoking PAM.
 * The missing Xrandr/Xlib-Cairo/PAM entry points are stubs; reaching one means
 * the supposedly non-locking preview tried to use a live display. */
#define main mel_program_main
#include "../src/main.c"
#undef main
#include <assert.h>
#include <sys/stat.h>

XRRMonitorInfo *XRRGetMonitors(Display *d, Window w, Bool active, int *n) {
    (void)d; (void)w; (void)active; (void)n; assert(!"preview opened Xrandr"); return NULL;
}
void XRRFreeMonitors(XRRMonitorInfo *m) { (void)m; assert(!"preview opened Xrandr"); }
cairo_surface_t *cairo_xlib_surface_create(Display *d, Drawable w, Visual *v, int a, int b) {
    (void)d; (void)w; (void)v; (void)a; (void)b; assert(!"preview opened Xlib Cairo"); return NULL;
}
int auth_check(const char *service, const char *name, const char *password) {
    (void)service; (void)name; (void)password; assert(!"preview attempted authentication"); return 0;
}

static unsigned long hash_file(const char *path) {
    FILE *fp = fopen(path, "rb"); unsigned long h = 2166136261u; int c;
    assert(fp);
    while ((c = fgetc(fp)) != EOF) h = (h ^ (unsigned)c) * 16777619u;
    fclose(fp); return h;
}
static void verify_size(const char *path, int w, int h) {
    cairo_surface_t *img = cairo_image_surface_create_from_png(path);
    assert(cairo_surface_status(img) == CAIRO_STATUS_SUCCESS);
    assert(cairo_image_surface_get_width(img) == w && cairo_image_surface_get_height(img) == h);
    cairo_surface_destroy(img);
}
/* Check the actual field renderer in idle, error and successful-unlock
 * states: each distinct style must keep visible security feedback. */
static unsigned long input_hash(int id, int state) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 100);
    cairo_t *cr = cairo_create(surface);
    lay_t l = { .ix = 10, .iy = 20, .iw = 300, .ih = 48 };
    frame_t f = { .t = now_ms(), .e_input = 1, .ghost = -1 };
    unsigned long h = 2166136261u; unsigned char *data; int n;
    layout_id = id; pwcount = 0; authenticating = 0; unlock_ok = state == 2;
    if (state == 1) f.shake_amt = 1;
    if (state == 2) f.unlock = 1;
    draw_input(cr, &l, &f, 1); cairo_destroy(cr);
    cairo_surface_flush(surface); data = cairo_image_surface_get_data(surface);
    n = cairo_image_surface_get_stride(surface) * cairo_image_surface_get_height(surface);
    for (int i = 0; i < n; i++) h = (h ^ data[i]) * 16777619u;
    cairo_surface_destroy(surface); unlock_ok = 0; return h;
}

static void layout_bounds(int id, int w, int h) {
    mon_t m = {0}; lay_t l;
    double cs, ds;
    m.w = w; m.h = h; ui_scale = 1; layout_id = id;
    cs = fmin(id == L_ZEN ? 100 : id == L_SPLIT ? 112 : 110, fmin(w * .20, h * (h < 360 ? .13 : .19)));
    ds = fmin(20, fmax(12, h * .045));
    compute_layout(&m, &l, cs, ds);
    assert(l.iw > 0 && l.ih > 0 && l.ix >= -w / 2.0 && l.ix + l.iw <= w / 2.0);
    assert(l.iy >= -h / 2.0 && l.iy + l.ih <= h / 2.0);
    assert(l.sy <= h / 2.0 && l.ly < l.iy);
    if (h < 360) assert(l.ly > l.cy + l.dy);
}

int main(void) {
    const char *layouts[] = {"split", "zen", "horizon"};
    const char *themes[] = {"my-dots", "rose-pine", "gruvbox", "everforest", "nord", "catppuccin", "tokyo-night"};
    unsigned long hashes[3], small_hashes[3]; char path[256], theme[256]; size_t i;
    lay_t compact[3];
    char home[] = "/tmp/mel-test-XXXXXX", dir[256], custom[256];
    FILE *fp;
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(setenv("XDG_CONFIG_HOME", home, 1) == 0);
    snprintf(dir, sizeof dir, "%s/mel", home); assert(mkdir(dir, 0700) == 0);
    snprintf(dir, sizeof dir, "%s/mel/themes", home); assert(mkdir(dir, 0700) == 0);
    snprintf(custom, sizeof custom, "%s/mel/themes/custom", home);
    fp = fopen(custom, "w"); assert(fp); fputs("accent_color = #123456\n", fp); fclose(fp);
    assert(!config_find_theme("custom", theme, sizeof theme)); assert(!strcmp(theme, custom));
    config_defaults(&cfg); anim = 1;
    for (i = 0; i < sizeof layouts / sizeof *layouts; i++) {
        int id = layout_by_name(layouts[i]);
        assert(id >= L_SPLIT && id < L_COUNT);
        layout_bounds(id, 320, 240); layout_bounds(id, 800, 600); layout_bounds(id, 1920, 1080);
        { mon_t m = { .w = 320, .h = 240 }; layout_id = id; ui_scale = 1;
          compute_layout(&m, &compact[i], 31.2, 12); }
        layout_id = id;
        snprintf(path, sizeof path, "%s/%s.png", home, layouts[i]);
        assert(preview_png(path, 1280, 720) == 0); verify_size(path, 1280, 720);
        hashes[i] = hash_file(path); unlink(path);
        snprintf(path, sizeof path, "%s/%s-small.png", home, layouts[i]);
        assert(preview_png(path, 320, 240) == 0); verify_size(path, 320, 240);
        small_hashes[i] = hash_file(path); unlink(path);
    }
    assert(hashes[0] != hashes[1] && hashes[1] != hashes[2] && hashes[0] != hashes[2]);
    assert(small_hashes[0] != small_hashes[1] && small_hashes[1] != small_hashes[2] && small_hashes[0] != small_hashes[2]);
    /* A short display must not silently collapse all layouts to one template. */
    assert(compact[0].align == 1 && compact[1].align == 0 && compact[2].align == 1);
    assert(compact[0].iy != compact[1].iy && compact[0].iy != compact[2].iy && compact[1].iy != compact[2].iy);
    for (int id = L_SPLIT; id <= L_HORIZON; id++) {
        unsigned long idle = input_hash(id, 0), failure = input_hash(id, 1), success = input_hash(id, 2);
        assert(idle != failure && idle != success && failure != success);
    }
    assert(layout_by_name("not-a-layout") == -1);
    assert(preview_size("320x240", &(int){0}, &(int){0}));
    assert(!preview_size("319x240", &(int){0}, &(int){0}));
    assert(!preview_size("1280x720junk", &(int){0}, &(int){0}));
    for (i = 0; i < sizeof themes / sizeof *themes; i++) {
        assert(!config_find_theme(themes[i], theme, sizeof theme));
        config_defaults(&cfg); assert(!config_load(&cfg, theme));
    }
    {
        char output[256];
        char *args[] = {"mel", "--layout", "split", "--theme", "my-dots", "--preview", output, "--preview-size", "800x600", NULL};
        snprintf(output, sizeof output, "%s/cli.png", home);
        optind = 0;
        assert(mel_program_main(9, args) == 0);
        verify_size(output, 800, 600); unlink(output);
        assert(layout_id == L_SPLIT && !strcmp(cfg.theme, "my-dots"));
    }
    {
        char config_path[256], output[256];
        char *args[] = {"mel", "--config", config_path, "--theme", "custom", "--layout", "horizon", "--preview", output, NULL};
        snprintf(output, sizeof output, "%s/config.png", home);
        snprintf(config_path, sizeof config_path, "%s/mel/config", home);
        fp = fopen(config_path, "w"); assert(fp);
        fputs("theme = nord\nlayout = zen\nclock_color = #ff0000\n", fp); fclose(fp);
        optind = 0;
        assert(mel_program_main(9, args) == 0);
        assert(layout_id == L_HORIZON && !strcmp(cfg.theme, "custom"));
        assert(fabs(cfg.accent_color.r - 0x12 / 255.0) < .001);
        assert(cfg.clock_color.r == 1 && cfg.clock_color.g == 0);
        unlink(config_path); unlink(output);
    }
    unlink(custom); snprintf(dir, sizeof dir, "%s/mel/themes", home); rmdir(dir);
    snprintf(dir, sizeof dir, "%s/mel", home); rmdir(dir); rmdir(home);
    puts("mel layouts, colour themes and non-locking PNG previews passed");
    return 0;
}
