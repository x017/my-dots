/* Font setup shared by the dashboard and the application launcher. */
#ifndef DWM_DASHBOARD_UI_FONTS_H
#define DWM_DASHBOARD_UI_FONTS_H

/* System monospace through fc-match, or DWM_DASHBOARD_FONT when set. */
static void load_monospace(cui_ctx *ui) {
    char path[2048], *end;
    char *cmd[] = {"fc-match", "-f", "%{file}\n", "monospace", NULL};
    const char *override = getenv("DWM_DASHBOARD_FONT");
    cui_font font = -1;
    if (override && *override) font = cui_font_load_file(ui, override);
    else if (run_command(cmd, NULL, path, sizeof path, 2000) == 0) {
        if ((end = strchr(path, '\n'))) *end = '\0';
        if (*path) font = cui_font_load_file(ui, path);
    }
    if (font < 0) font = CUI_FONT_MONO;
    cui_font_set_role(ui, CUI_FONT_BODY, font);
    cui_font_set_role(ui, CUI_FONT_MEDIUM, font);
    cui_font_set_role(ui, CUI_FONT_DISPLAY, font);
}

static void ui_fonts_setup(cui_ctx *ui) {
    load_monospace(ui);
    cui_persian_fallback(ui);
}
#endif
