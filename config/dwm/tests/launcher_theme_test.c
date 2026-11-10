/* Rasterizes the launcher's command palette headlessly with CUI's software
 * renderer and checks the pixels: the window must be one flat surface, with no
 * scrim, no panel edge and no shadow ring around the palette. Runs the real
 * launcher_theme() from dashboard/launcher_theme.h, so a change there shows up
 * here. After building dashboard/build, compile and run with:
 * cc -std=c99 -Iconfig/dwm/dashboard/build/_deps/cui-src/include \
 *   config/dwm/tests/launcher_theme_test.c \
 *   config/dwm/dashboard/build/_deps/cui-build/libcui.a \
 *   -lX11 -lm -pthread -ldl -o /tmp/launcher_theme_test
 * /tmp/launcher_theme_test [optional-output.png] */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include "../dashboard/launcher_theme.h"

#define W 660
#define H 520

static const cui_command commands[] = {
    { "Firefox", CUI_ICON_GLOBE, "Network", "firefox" },
    { "Ghostty", CUI_ICON_TERMINAL, "System", "ghostty" },
    { "GIMP", CUI_ICON_IMAGE, "Graphics", "gimp" },
    { "Files", CUI_ICON_FILE_TEXT, "Utility", "nautilus" },
};

/* One frame of the launcher, exactly as launcher.c draws it. */
static void frame(cui_ctx *ui, bool *open) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_spacer(ui);
    }
    cui_command_palette(ui, open, commands, (int)(sizeof commands / sizeof *commands));
}

static unsigned char *rasterize(bool launcher_style, const char *png) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK });
    unsigned char *px;
    const cui_draw_data *dd;
    bool open = true;
    int f;

    cui_set_viewport(ui, W, H, 1.0f);
    if (launcher_style) launcher_theme(ui);
    /* Enough frames for the palette's enter spring to settle. */
    for (f = 0; f < 90; f++) { cui_begin_frame(ui, 1.0f / 60); frame(ui, &open); cui_end_frame(ui); }
    dd = cui_get_draw_data(ui);
    px = malloc((size_t)dd->width_px * (size_t)dd->height_px * 4);
    cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    if (png) cui_write_png(png, px, dd->width_px, dd->height_px, dd->width_px * 4);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    return px;
}

static void at(const unsigned char *px, int x, int y, int *r, int *g, int *b) {
    const unsigned char *p = px + ((size_t)y * W + (size_t)x) * 4;
    *r = p[0]; *g = p[1]; *b = p[2];
}

int main(int argc, char **argv) {
    unsigned char *plain = rasterize(false, NULL);
    unsigned char *styled = rasterize(true, argc > 1 ? argv[1] : NULL);
    int r, g, b, x, y, different = 0, flat = 1;

    /* The panel fill the launcher uses for the whole window. */
    const int sr = 0x18, sg = 0x1a, sb = 0x1e;

    /* Without the launcher theme the palette is a box on a scrimmed window: the
     * corner is darker than the panel. Sanity check that the two renders differ,
     * so the assertions below cannot pass on an empty frame. */
    at(plain, 4, 4, &r, &g, &b);
    if (r != sr || g != sg || b != sb) different = 1;
    if (!different) { fprintf(stderr, "expected the unstyled render to show a scrim\n"); return 1; }

    /* With it, the window edge is exactly the panel fill: no scrim, no shadow. */
    at(styled, 4, 4, &r, &g, &b);
    if (r != sr || g != sg || b != sb) {
        fprintf(stderr, "window corner is #%02x%02x%02x, expected #181a1e\n", r, g, b);
        return 1;
    }
    /* Every pixel of the outer 24 px frame must be that same fill, which is what
     * makes the panel boundary invisible. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            if (x >= 24 && x < W - 24 && y >= 24 && y < H - 24) continue;
            at(styled, x, y, &r, &g, &b);
            if (r != sr || g != sg || b != sb) {
                fprintf(stderr, "pixel %d,%d is #%02x%02x%02x, expected #181a1e\n", x, y, r, g, b);
                flat = 0; y = H; break;
            }
        }
    }
    if (!flat) return 1;
    /* Sample the panel boundary and empty footer area too: a flat outer frame
     * alone would not catch a dark box wholly inside that frame. */
    {
        const int points[][2] = { {40, 200}, {41, 200}, {42, 200},
                                  {330, 73}, {330, 74}, {330, 75},
                                  {600, 440}, {610, 440} };
        size_t i;
        for (i = 0; i < sizeof points / sizeof *points; i++) {
            at(styled, points[i][0], points[i][1], &r, &g, &b);
            /* Rounded edge antialiasing can differ by a few intensity levels. */
            if (abs(r - sr) > 3 || abs(g - sg) > 3 || abs(b - sb) > 3) {
                fprintf(stderr, "panel pixel %d,%d is #%02x%02x%02x, expected #181a1e\n",
                        points[i][0], points[i][1], r, g, b);
                return 1;
            }
        }
    }
    /* And the palette itself must still have drawn content. */
    {
        int i, changed = 0;
        for (i = 0; i < W * H; i++) {
            const unsigned char *p = styled + (size_t)i * 4;
            if (p[0] != sr || p[1] != sg || p[2] != sb) { changed = 1; break; }
        }
        if (!changed) { fprintf(stderr, "the palette drew nothing\n"); return 1; }
    }
    free(plain);
    free(styled);
    puts("launcher window renders as one flat surface with the palette on it");
    return 0;
}
