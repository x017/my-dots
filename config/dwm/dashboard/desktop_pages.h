/* Shared CUI display page. */
static void display_page(cui_ctx *ui, UIState *state, const DesktopState *d, bool busy) {
    int i, shown = 0;
    if (!state->display_output[0] && d->output_count) copystr(state->display_output, sizeof state->display_output, d->outputs[0]);
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_text(ui, "Displays", .font = CUI_FONT_MEDIUM, .size = 16);
        cui_secondary(ui, d->xrandr_available ? "Choose a monitor to manage its resolution and refresh rate." : "xrandr is unavailable.");
        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
            for (i = 0; i < d->output_count; i++) {
                cui_id_scope(ui, d->outputs[i]) {
                    if (cui_button(ui, d->outputs[i], .size = CUI_SIZE_SM, .selected = !strcmp(state->display_output, d->outputs[i])))
                        copystr(state->display_output, sizeof state->display_output, d->outputs[i]);
                }
            }
        }
        if (!d->output_count && d->xrandr_available) cui_secondary(ui, "No connected monitors found.");
    }
    cui_column(ui, .width = CUI_FILL, .gap = 3) {
        cui_text(ui, "Resolution & refresh rate", .font = CUI_FONT_MEDIUM, .size = 15);
        cui_secondary(ui, "Successful changes are restored at the next session.");
    }
    for (i = 0; i < d->mode_count; i++) {
        const DisplayMode *m = &d->modes[i]; char params[100], id[32];
        if (strcmp(m->output, state->display_output)) continue;
        shown++; snprintf(id, sizeof id, "mode-%d", i);
        cui_id_scope(ui, id) {
            cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10) {
                    cui_column(ui, .width = CUI_FILL, .gap = 4) {
                        cui_text(ui, m->mode, .font = CUI_FONT_MEDIUM, .size = 14);
                        cui_textf(ui, "%s Hz  ·  %s", m->rate, m->current ? "Currently active" : "Available");
                    }
                    if (cui_button(ui, m->current ? "Active" : "Apply", .size = CUI_SIZE_SM, .disabled = busy || m->current)) {
                        snprintf(params, sizeof params, "%s %s", m->mode, m->rate);
                        request_action(ACT_DISPLAY_APPLY, m->output, params, 0);
                    }
                }
            }
        }
    }
    if (!shown) cui_card(ui, .width = CUI_FILL, .pad = 16) {
        cui_secondary(ui, "No modes available for this monitor.");
    }
}
