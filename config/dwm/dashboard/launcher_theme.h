/* The launcher's take on the shared palette.
 *
 * CUI's command palette is an overlay: it floats a rounded panel over a scrim.
 * That suits an app with content behind it, but the launcher window holds
 * nothing else, so the panel would just be a dark box sitting inside the
 * window. Making the window itself the panel — one flat surface, no edge, no
 * scrim, no shadow — leaves the search field and list sitting directly on the
 * window. Kept in its own header so tests/launcher_theme_test.c can rasterize
 * the result headlessly and check the pixels. */
#ifndef DWM_LAUNCHER_THEME_H
#define DWM_LAUNCHER_THEME_H
#include "theme.h"

static void launcher_theme(cui_ctx *ui) {
    cui_theme *t;
    style(ui);
    t = cui_theme_get(ui);
    t->colors[CUI_TOKEN_BACKGROUND] = t->colors[CUI_TOKEN_SURFACE_RAISED];
    t->colors[CUI_TOKEN_BORDER] = t->colors[CUI_TOKEN_SURFACE_RAISED];
    t->colors[CUI_TOKEN_SURFACE_MUTED] = t->colors[CUI_TOKEN_SURFACE_RAISED];
    t->colors[CUI_TOKEN_SCRIM] = cui_alpha(t->colors[CUI_TOKEN_SCRIM], 0.0f);
    t->shadow_floating[0].alpha = 0.0f;
    t->shadow_floating[1].alpha = 0.0f;
}

#endif
