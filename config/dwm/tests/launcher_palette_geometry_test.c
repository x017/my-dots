/* Exercise the actual pinned CUI palette headlessly. The launcher uses its
 * panel's layout rectangle for the X11 window shape; it must remain available
 * and shrink when search filters the list. */
#include <assert.h>
#include <stdio.h>
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <cui/cui_app.h>
#include "../dashboard/launcher_theme.h"
#include "../dashboard/launcher_shape.h"

/* Fake the X11 requests so the window shape itself can be checked without an
 * X server. The CUI layout below is real. */
static int shape_calls;
static unsigned width_px = 1320, height_px = 1040;
Status XGetGeometry(Display *d, Drawable w, Window *root, int *x, int *y,
                    unsigned int *width, unsigned int *height,
                    unsigned int *border, unsigned int *depth) {
    (void)d; (void)w; *root = 1; *x = *y = 0;
    *width = width_px; *height = height_px; *border = 0; *depth = 24;
    return 1;
}
void XShapeCombineRectangles(Display *d, Window w, int kind, int x, int y,
                             XRectangle *rects, int n, int op, int ordering) {
    (void)d; (void)w; (void)x; (void)y; (void)ordering;
    assert(kind == ShapeBounding && op == ShapeSet && n > 1);
    assert(rects[0].width < rects[n - 1].width); /* rounded corners */
    shape_calls++;
}
int XFlush(Display *d) { (void)d; return 0; }

static const cui_command commands[] = {
    { "Firefox", CUI_ICON_GLOBE, "Network", NULL },
    { "Ghostty", CUI_ICON_TERMINAL, "System", NULL },
    { "Files", CUI_ICON_FILE_TEXT, "Utility", NULL },
};

static void draw(cui_ctx *ui, bool *open) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { cui_spacer(ui); }
    cui_command_palette(ui, open, commands, 3);
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK });
    bool open = true;
    cui_rect full, filtered;
    struct launcher_shape shape = {0};
    int i;
    assert(sw && ui);
    launcher_theme(ui);
    cui_set_viewport(ui, 660, 520, 1);
    cui_input_focus(ui, true);
    for (i = 0; i < 80; i++) {
        cui_begin_frame(ui, 1.0f / 60); draw(ui, &open); cui_end_frame(ui);
    }
    full = cui_get_rect(ui, cui_make_id(ui, "#command-palette") ^ 0x9A4Eu);
    assert(full.x > 0 && full.y > 0 && full.w > 0 && full.h > 0);
    assert(full.x + full.w < 660 && full.y + full.h < 520);
    shape.display = (Display *)1; shape.window = 123;
    launcher_shape_update(&shape, ui);
    assert(shape_calls == 1 && shape.last.x == 80 && shape.last.width == 1160);
    launcher_shape_update(&shape, ui);
    assert(shape_calls == 1); /* unchanged geometry does not issue another request */
    cui_input_text(ui, "Firefox");
    for (i = 0; i < 3; i++) {
        cui_begin_frame(ui, 1.0f / 60); draw(ui, &open); cui_end_frame(ui);
    }
    filtered = cui_get_rect(ui, cui_make_id(ui, "#command-palette") ^ 0x9A4Eu);
    assert(filtered.w == full.w && filtered.h > 0 && filtered.h < full.h);
    launcher_shape_update(&shape, ui);
    assert(shape_calls == 2 && shape.last.height < (unsigned)(full.h * 2));
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0);
    cui_begin_frame(ui, 1.0f / 60); draw(ui, &open); cui_end_frame(ui);
    assert(!open);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    puts("palette shape follows filtering and scaling; Escape closes it");
    return 0;
}
