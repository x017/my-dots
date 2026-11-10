/* Restrict the X11 launcher window to CUI's actual command-palette panel.
 * The upstream palette is a modal overlay with a full-window scrim and fixed
 * margins. Merely matching the colours leaves a large rectangular window.
 * CUI is pinned to a known revision; its panel id is derived from the palette
 * id with 0x9A4E in cui_overlay.c. Read its most recent layout each frame so
 * the shape also follows the panel when filtering changes its height. */
#ifndef DWM_LAUNCHER_SHAPE_H
#define DWM_LAUNCHER_SHAPE_H

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

struct launcher_shape {
    Display *display;
    Window window;
    XRectangle last;
    int have_last;
};

static inline void launcher_shape_open(struct launcher_shape *shape, cui_app *app) {
    int event_base, error_base;
    memset(shape, 0, sizeof *shape);
    if (strcmp(cui_app_backend(app), "x11") != 0) return;
    shape->display = XOpenDisplay(NULL);
    if (!shape->display) return;
    if (!XShapeQueryExtension(shape->display, &event_base, &error_base)) {
        XCloseDisplay(shape->display);
        shape->display = NULL;
        return;
    }
    shape->window = (Window)(uintptr_t)cui_app_native_window(app);
}

static inline void launcher_shape_update(struct launcher_shape *shape, cui_ctx *ui) {
    cui_rect panel;
    cui_vec2 viewport;
    Window root;
    int x, y;
    unsigned int width, height, border, depth;
    float sx, sy;
    XRectangle rect, rows[257];
    int radius, i, n;
    if (!shape->display || !shape->window) return;
    panel = cui_get_rect(ui, cui_make_id(ui, "#command-palette") ^ 0x9A4Eu);
    viewport = cui_viewport_size(ui);
    if (panel.w <= 0 || panel.h <= 0 || viewport.x <= 0 || viewport.y <= 0) return;
    if (!XGetGeometry(shape->display, shape->window, &root, &x, &y,
                      &width, &height, &border, &depth)) return;
    sx = (float)width / viewport.x;
    sy = (float)height / viewport.y;
    rect.x = (short)floorf(panel.x * sx);
    rect.y = (short)floorf(panel.y * sy);
    rect.width = (unsigned short)(ceilf((panel.x + panel.w) * sx) - rect.x);
    rect.height = (unsigned short)(ceilf((panel.y + panel.h) * sy) - rect.y);
    if (!rect.width || !rect.height) return;
    if (!shape->have_last || rect.x != shape->last.x || rect.y != shape->last.y ||
        rect.width != shape->last.width || rect.height != shape->last.height) {
        /* Match the panel's rounded silhouette rather than exposing the
         * surrounding window at its corners. The middle is one rectangle;
         * the two rounded ends are made of one-pixel-high strips. */
        radius = (int)ceilf(cui_theme_get(ui)->radius_panel * fminf(sx, sy));
        if (radius > 128) radius = 128;
        if (radius > rect.width / 2) radius = rect.width / 2;
        if (radius > rect.height / 2) radius = rect.height / 2;
        n = 0;
        for (i = 0; i < radius; i++) {
            float dy = radius - i - 0.5f;
            int inset = (int)ceilf(radius - sqrtf(radius * radius - dy * dy));
            rows[n++] = (XRectangle){ (short)(rect.x + inset), (short)(rect.y + i),
                                      (unsigned short)(rect.width - 2 * inset), 1 };
            rows[n++] = (XRectangle){ (short)(rect.x + inset), (short)(rect.y + rect.height - i - 1),
                                      (unsigned short)(rect.width - 2 * inset), 1 };
        }
        if (rect.height > 2 * radius)
            rows[n++] = (XRectangle){ rect.x, (short)(rect.y + radius), rect.width,
                                      (unsigned short)(rect.height - 2 * radius) };
        XShapeCombineRectangles(shape->display, shape->window, ShapeBounding,
                                0, 0, rows, n, ShapeSet, Unsorted);
        XFlush(shape->display);
        shape->last = rect;
        shape->have_last = 1;
    }
}

static inline void launcher_shape_close(struct launcher_shape *shape) {
    if (shape->display) XCloseDisplay(shape->display);
}
#endif
