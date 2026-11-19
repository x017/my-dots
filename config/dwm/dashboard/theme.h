/* Shared colour palette: the native Alt+Tab switcher palette in dwm.c.
 * Used by the dashboard, the bar popup and the application launcher so the
 * three match. */
#ifndef DWM_DASHBOARD_THEME_H
#define DWM_DASHBOARD_THEME_H

static void style(cui_ctx *ui) {
    cui_theme *t = cui_theme_get(ui);
    t->colors[CUI_TOKEN_BACKGROUND] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_SURFACE] = CUI_RGB(0x121417);
    t->colors[CUI_TOKEN_SURFACE_RAISED] = CUI_RGB(0x181a1e);
    t->colors[CUI_TOKEN_SURFACE_MUTED] = CUI_RGB(0x121417);
    t->colors[CUI_TOKEN_FOREGROUND] = CUI_RGB(0xadbac7);
    t->colors[CUI_TOKEN_TEXT_SECONDARY] = CUI_RGB(0x8a949e);
    t->colors[CUI_TOKEN_TEXT_MUTED] = CUI_RGB(0x8a949e);
    t->colors[CUI_TOKEN_BORDER] = CUI_RGB(0x1e2025);
    t->colors[CUI_TOKEN_BORDER_SUBTLE] = CUI_RGB(0x1e2025);
    t->colors[CUI_TOKEN_BORDER_STRONG] = CUI_RGB(0x2d3035);
    t->colors[CUI_TOKEN_ACCENT] = CUI_RGB(0xe9eff6);
    t->colors[CUI_TOKEN_ACCENT_STRONG] = CUI_RGB(0xe9eff6);
    t->colors[CUI_TOKEN_ACCENT_SUBTLE] = CUI_RGB(0x2d3035);
    t->colors[CUI_TOKEN_ACCENT_FOREGROUND] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_CONTROL_ON] = CUI_RGB(0x7fa8c9);
    t->colors[CUI_TOKEN_CONTROL_GLYPH] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_CONTROL_THUMB_ON] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_DANGER] = CUI_RGB(0xb86a6a);
    t->colors[CUI_TOKEN_INFO] = CUI_RGB(0x7fa8c9);
    t->colors[CUI_TOKEN_HOVER] = CUI_RGB(0x181a1e);
    t->colors[CUI_TOKEN_FOCUS_RING] = CUI_RGB(0x7fa8c9);
    t->radius_control = 8; t->radius_panel = 12; t->radius_surface = 12;
    t->text_body = 13; t->heading[2] = 18;
}


#endif
