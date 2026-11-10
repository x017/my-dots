/* Prefer installed Vazirmatn or Vazir for UI text; preserve icon and fallback fonts. */
#ifndef DWM_CUI_PERSIAN_FONT_H
#define DWM_CUI_PERSIAN_FONT_H
static void cui_persian_fallback(cui_ctx *ui, int keep_text_font) {
    static int searched;
    static char path[2048], preferred_path[2048];
    const char *override = getenv("CUI_PERSIAN_FONT");
    cui_font font = -1;
    if (!searched) {
        char *end;
        char result[4096];
        const char *families[] = {"Vazirmatn", "Vazir"};
        char *vazir_cmd[] = {"fc-match", "-f", "%{family[0]}\n%{file}\n", NULL, NULL};
        char *cmd[] = {"fc-match", "-f", "%{file}\n", ":lang=fa", NULL};
        const char *common[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
        searched = 1;
        for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
            char query[64];
            snprintf(query, sizeof query, "%s:style=Regular", families[i]);
            vazir_cmd[3] = query;
            if (run_command(vazir_cmd, NULL, result, sizeof result, 2000) != 0 ||
                !(end = strchr(result, '\n'))) continue;
            *end++ = '\0';
            if (!strcasecmp(result, families[i])) {
                char *newline = strchr(end, '\n');
                if (newline) *newline = '\0';
                if (strlen(end) < sizeof preferred_path && access(end, R_OK) == 0) {
                    strcpy(preferred_path, end);
                    break;
                }
            }
        }
        if (run_command(cmd, NULL, path, sizeof path, 2000) == 0) {
            if ((end = strchr(path, '\n'))) *end = '\0';
        }
        if (!path[0] || access(path, R_OK) != 0) {
            path[0] = '\0';
            for (size_t i = 0; i < sizeof common / sizeof common[0]; i++)
                if (access(common[i], R_OK) == 0) { snprintf(path, sizeof path, "%s", common[i]); break; }
        }
        if (!path[0]) fprintf(stderr, "CUI: install a Persian-capable font (e.g. DejaVu Sans) or set CUI_PERSIAN_FONT\n");
    }
    if (override && *override) font = cui_font_load_file(ui, override);
    if (font < 0 && path[0]) font = cui_font_load_file(ui, path);
    if (font >= 0) {
        cui_font_add_fallback(ui, CUI_FONT_BODY, font);
        cui_font_add_fallback(ui, CUI_FONT_MEDIUM, font);
        cui_font_add_fallback(ui, CUI_FONT_DISPLAY, font);
        cui_font_add_fallback(ui, CUI_FONT_MONO, font);
    }
    if (!keep_text_font && preferred_path[0]) {
        cui_font preferred = cui_font_load_file(ui, preferred_path);
        if (preferred >= 0) {
            cui_font_add_fallback(ui, preferred, CUI_FONT_MONO);
            if (font >= 0) cui_font_add_fallback(ui, preferred, font);
            cui_font_set_role(ui, CUI_FONT_BODY, preferred);
            cui_font_set_role(ui, CUI_FONT_MEDIUM, preferred);
            cui_font_set_role(ui, CUI_FONT_DISPLAY, preferred);
            cui_font_set_role(ui, CUI_FONT_MONO, preferred);
        }
    }
}
#endif
