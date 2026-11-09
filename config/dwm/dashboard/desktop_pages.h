/* Shared CUI display and wallpaper pages. */
static void display_page(cui_ctx *ui, UIState *state, const DesktopState *d, bool busy) {
    int i, shown = 0;
    if (!state->display_output[0] && d->output_count) copystr(state->display_output, sizeof state->display_output, d->outputs[0]);
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
        cui_label(ui, "Displays");
        cui_secondary(ui, d->xrandr_available ? "Choose a connected monitor and a resolution / refresh rate." : "xrandr is unavailable.");
        cui_row(ui, .width = CUI_FILL, .gap = 6, .wrap = true) {
            for (i = 0; i < d->output_count; i++) {
                cui_id_scope(ui, d->outputs[i]) {
                    if (cui_button(ui, d->outputs[i], .size = CUI_SIZE_SM, .selected = !strcmp(state->display_output, d->outputs[i])))
                        copystr(state->display_output, sizeof state->display_output, d->outputs[i]);
                }
            }
        }
        if (!d->output_count && d->xrandr_available) cui_secondary(ui, "No connected monitors found.");
    }
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
        cui_label(ui, "Resolution & refresh rate");
        for (i = 0; i < d->mode_count; i++) {
            const DisplayMode *m = &d->modes[i]; char title[128], params[100], id[32];
            if (strcmp(m->output, state->display_output)) continue;
            shown++; snprintf(title, sizeof title, "%s  /  %s Hz%s", m->mode, m->rate, m->current ? "  ·  Current" : "");
            snprintf(id, sizeof id, "mode-%d", i);
            cui_id_scope(ui, id) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                    cui_column(ui, .width = CUI_FILL) { cui_text(ui, title); }
                    if (cui_button(ui, "Apply", .size = CUI_SIZE_SM, .disabled = busy || m->current)) {
                        snprintf(params, sizeof params, "%s %s", m->mode, m->rate);
                        request_action(ACT_DISPLAY_APPLY, m->output, params, 0);
                    }
                }
            }
        }
        if (!shown) cui_secondary(ui, "No modes available for this monitor.");
        cui_secondary(ui, "Successful changes are saved for the next dwm session.");
    }
}
static void wallpaper_page(cui_ctx *ui, UIState *state, const DesktopState *d, bool busy) {
    int i;
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
        cui_label(ui, "Wallpapers");
        cui_secondary(ui, "Images from DWM_WALLPAPERS_DIR, or ~/Downloads/wallpaper.");
        if (!d->wallpaper_available) cui_secondary(ui, "Wallpaper folder unavailable. Create it and reopen this page.");
        else if (!d->wallpaper_count) cui_secondary(ui, "No supported images in the wallpaper folder.");
        for (i = 0; i < d->wallpaper_count; i++) {
            const char *name = d->wallpaper_names[i];
            cui_id_scope(ui, name) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                    cui_column(ui, .width = CUI_FILL) { cui_text(ui, name); }
                    if (cui_button(ui, "Apply", .size = CUI_SIZE_SM, .disabled = busy)) {
                        copystr(state->wallpaper, sizeof state->wallpaper, name);
                        request_action(ACT_WALLPAPER_APPLY, NULL, name, 0);
                    }
                }
            }
        }
        cui_secondary(ui, "Uses feh and saves the wallpaper for your next session.");
    }
}
