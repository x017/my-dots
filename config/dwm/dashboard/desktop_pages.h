/* Shared CUI display page: a to-scale layout map plus full per-monitor
 * settings. Shared by the optional CUI dashboard and the bar's popup. */
#include <limits.h>

typedef struct { const DesktopState *d; int selected; } DisplayMap;

/* One rectangle per connected output, scaled to fit, positioned exactly as
 * xrandr reports them. An output that is switched off has no geometry. */
static int display_map_bounds(const DesktopState *d, int *minx, int *miny, int *maxx, int *maxy) {
    int i, found = 0;
    *minx = *miny = INT_MAX; *maxx = *maxy = INT_MIN;
    for (i = 0; i < d->output_count; i++) {
        const DisplayOutput *o = &d->outputs[i];
        if (!o->active || o->w <= 0 || o->h <= 0) continue;
        found = 1;
        if (o->x < *minx) *minx = o->x;
        if (o->y < *miny) *miny = o->y;
        if (o->x + o->w > *maxx) *maxx = o->x + o->w;
        if (o->y + o->h > *maxy) *maxy = o->y + o->h;
    }
    return found;
}

/* Shared by the painter and by click testing, so both agree. Rectangles are in
 * the same coordinate space as the canvas rectangle that was passed in. */
static cui_rect display_map_rect(const DesktopState *d, cui_rect area, int i) {
    cui_rect r = {0, 0, 0, 0};
    int minx, miny, maxx, maxy;
    float scale, spanx, spany, ox, oy;
    const DisplayOutput *o;
    if (i < 0 || i >= d->output_count) return r;
    o = &d->outputs[i];
    if (!o->active || o->w <= 0 || o->h <= 0) return r;
    if (!display_map_bounds(d, &minx, &miny, &maxx, &maxy)) return r;
    spanx = (float)(maxx - minx); spany = (float)(maxy - miny);
    if (spanx <= 0 || spany <= 0 || area.w < 20 || area.h < 34) return r;
    scale = (area.w - 16) / spanx;
    if ((area.h - 34) / spany < scale) scale = (area.h - 34) / spany;
    if (scale <= 0) return r;
    ox = area.x + 8 + ((area.w - 16) - spanx * scale) / 2;
    oy = area.y + 8 + ((area.h - 34) - spany * scale) / 2;
    r.x = ox + (o->x - minx) * scale; r.y = oy + (o->y - miny) * scale;
    r.w = o->w * scale; r.h = o->h * scale;
    return r;
}

static void display_map_paint(cui_ctx *ui, cui_rect area, void *user) {
    const DisplayMap *map = (const DisplayMap *)user;
    int i, off = 0;
    cui_draw_rect(ui, area, CUI_CORNERS(10), CUI_RGB(0x0e1013));
    for (i = 0; i < map->d->output_count; i++) {
        const DisplayOutput *o = &map->d->outputs[i];
        cui_rect r = display_map_rect(map->d, area, i);
        int selected = i == map->selected;
        if (r.w <= 0) {
            /* Switched-off monitors get a strip along the bottom of the map. */
            cui_rect s = { area.x + 8 + off * 96, area.y + area.h - 20, 88, 14 };
            cui_draw_rect_border(ui, s, CUI_CORNERS(4), CUI_RGB(0x121417),
                                 selected ? CUI_RGB(0x7fa8c9) : CUI_RGB(0x2d3035), selected ? 2 : 1);
            cui_draw_text(ui, CUI_FONT_MONO, 9, (cui_vec2){ s.x + 6, s.y + 2 },
                          selected ? CUI_RGB(0xe9eff6) : CUI_RGB(0x8a949e), o->name, -1);
            off++;
            continue;
        }
        cui_draw_rect_border(ui, r, CUI_CORNERS(5),
                             selected ? CUI_RGB(0x24313d) : CUI_RGB(0x181a1e),
                             selected ? CUI_RGB(0x7fa8c9) : CUI_RGB(0x2d3035), selected ? 2 : 1);
        cui_draw_text(ui, CUI_FONT_MONO, 10, (cui_vec2){ r.x + 7, r.y + 5 },
                      selected ? CUI_RGB(0xe9eff6) : CUI_RGB(0xadbac7), o->name, -1);
        if (o->primary)
            cui_draw_text(ui, CUI_FONT_MONO, 9, (cui_vec2){ r.x + 7, r.y + 18 }, CUI_RGB(0x7fa8c9), "PRIMARY", -1);
        if (strcmp(o->rotation, "normal"))
            cui_draw_text(ui, CUI_FONT_MONO, 9, (cui_vec2){ r.x + 7, r.y + 30 }, CUI_RGB(0x8a949e), o->rotation, -1);
    }
    cui_draw_text(ui, CUI_FONT_MONO, 9, (cui_vec2){ area.x + 8, area.y + area.h - 15 },
                  CUI_RGB(0x8a949e), "click a monitor to manage it", -1);
}

static void display_page(cui_ctx *ui, UIState *state, const DesktopState *d, bool busy) {
    static const char *const layout_items[] = { "Extend", "Mirror" };
    static const char *const relations[] = { "left-of", "right-of", "above", "below" };
    static const char *const relation_labels[] = { "Place left", "Place right", "Place above", "Place below" };
    static const char *const rotate_items[] = { "Normal", "Left", "Right", "Inverted" };
    static const char *const rotations[] = { "normal", "left", "right", "inverted" };
    static const char *const scale_items[] = { "100%", "125%", "150%", "175%", "200%" };
    static const char *const scales[] = { "1x1", "0.8x0.8", "0.6667x0.6667", "0.5714x0.5714", "0.5x0.5" };
    const DisplayOutput *cur = NULL;
    const char *others[DESKTOP_OUTPUTS];
    char id[160], params[160], heading[192], status[192];
    int i, selected = 0, shown = 0, others_count = 0, target = 0, rotate = 0, mapped = 0;
    int minx, miny, maxx, maxy;

    if (!state->display_output[0] && d->output_count) copystr(state->display_output, sizeof state->display_output, d->outputs[0].name);
    for (i = 0; i < d->output_count; i++)
        if (!strcmp(d->outputs[i].name, state->display_output)) { selected = i; cur = &d->outputs[i]; break; }
    if (!cur && d->output_count) { selected = 0; cur = &d->outputs[0]; copystr(state->display_output, sizeof state->display_output, cur->name); }
    for (i = 0; i < d->output_count; i++)
        if (cur && strcmp(d->outputs[i].name, cur->name)) others[others_count++] = d->outputs[i].name;
    for (i = 0; i < others_count; i++)
        if (!strcmp(others[i], state->display_target)) { target = i; break; }
    if (!others_count) target = 0;
    mapped = d->output_count > 1 && display_map_bounds(d, &minx, &miny, &maxx, &maxy);

    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_text(ui, "Displays", .font = CUI_FONT_MEDIUM, .size = 16);
        cui_secondary(ui, d->xrandr_available ? "The map shows how the monitors are arranged." : "xrandr is unavailable.");
        if (mapped) {
            DisplayMap map = { d, selected };
            cui_canvas_result res = cui_canvas(ui, "display-map", CUI_FILL, CUI_PX(190), display_map_paint, &map, sizeof map);
            /* cui_canvas reports the mouse relative to its own rectangle. */
            if (res.clicked) for (i = 0; i < d->output_count; i++) {
                cui_rect r = display_map_rect(d, res.rect, i);
                if (r.w > 0 && res.mouse.x >= r.x - res.rect.x && res.mouse.x <= r.x - res.rect.x + r.w &&
                    res.mouse.y >= r.y - res.rect.y && res.mouse.y <= r.y - res.rect.y + r.h) {
                    copystr(state->display_output, sizeof state->display_output, d->outputs[i].name); break;
                }
            }
        } else if (d->output_count == 1 && cur) {
            cui_secondary(ui, "One monitor connected; arrangement needs a second one.");
        }
        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
            for (i = 0; i < d->output_count; i++) {
                cui_id_scope(ui, d->outputs[i].name) {
                    snprintf(id, sizeof id, "%s%s%s", d->outputs[i].name, d->outputs[i].primary ? " · primary" : "",
                             d->outputs[i].active ? "" : " · off");
                    if (cui_button(ui, id, .size = CUI_SIZE_SM, .selected = i == selected))
                        copystr(state->display_output, sizeof state->display_output, d->outputs[i].name);
                }
            }
        }
        if (!d->output_count && d->xrandr_available) cui_secondary(ui, "No connected monitors found.");
    }

    if (cur) {
        snprintf(heading, sizeof heading, "%s · arrangement", cur->name);
        if (cur->active) snprintf(status, sizeof status, "%d × %d at %d,%d  ·  rotated %s", cur->w, cur->h, cur->x, cur->y, cur->rotation);
        else snprintf(status, sizeof status, "Switched off  ·  rotated %s", cur->rotation);
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
            cui_text(ui, heading, .font = CUI_FONT_MEDIUM, .size = 15);
            cui_text(ui, status, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_secondary(ui, "Arrangement, rotation, scale and power apply immediately and are not restored at login.");
            cui_disabled_scope(ui, busy) {
                if (others_count) {
                    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10, .wrap = true) {
                        cui_segmented(ui, "layout-mode", layout_items, 2, &state->display_layout, .size = CUI_SIZE_SM);
                        cui_select(ui, "arrange-target", others, others_count, &target, .width = CUI_PX(150));
                    }
                    if (state->display_layout) {
                        if (cui_button(ui, "Mirror these two monitors", .size = CUI_SIZE_SM)) {
                            snprintf(params, sizeof params, "same-as %s", others[target]);
                            request_action(ACT_DISPLAY_ARRANGE, cur->name, params, 0);
                        }
                        cui_secondary(ui, "Both show the same picture; the second one follows the first.");
                    } else {
                        cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
                            for (i = 0; i < 4; i++) {
                                cui_id_scope(ui, relations[i]) {
                                    if (cui_button(ui, relation_labels[i], .size = CUI_SIZE_SM)) {
                                        snprintf(params, sizeof params, "%s %s", relations[i], others[target]);
                                        request_action(ACT_DISPLAY_ARRANGE, cur->name, params, 0);
                                    }
                                }
                            }
                        }
                        cui_secondary(ui, "Extends the desktop: each monitor keeps its own picture.");
                    }
                } else {
                    cui_secondary(ui, "Connect a second monitor to mirror or extend.");
                }
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10, .wrap = true) {
                    for (i = 0; i < 4; i++) if (!strcmp(cur->rotation, rotations[i])) rotate = i;
                    cui_text(ui, "Rotation", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                    if (cui_segmented(ui, "rotation", rotate_items, 4, &rotate, .size = CUI_SIZE_SM))
                        request_action(ACT_DISPLAY_ROTATE, cur->name, rotations[rotate], 0);
                }
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10, .wrap = true) {
                    cui_text(ui, "UI scale", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_segmented(ui, "scale-preset", scale_items, 5, &state->display_scale, .size = CUI_SIZE_SM);
                    if (cui_button(ui, "Apply scale", .size = CUI_SIZE_SM))
                        request_action(ACT_DISPLAY_SCALE, cur->name, scales[state->display_scale % 5], 0);
                }
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 18, .wrap = true) {
                    bool primary = cur->primary != 0, enabled = cur->active != 0;
                    if (cui_switch(ui, "Primary monitor", &primary, .disabled = cur->primary != 0))
                        request_action(ACT_DISPLAY_PRIMARY, cur->name, "", 0);
                    if (cui_switch(ui, "Enabled", &enabled, .description = "Off keeps the monitor connected but blanked."))
                        request_action(ACT_DISPLAY_POWER, cur->name, "", enabled ? 1 : 0);
                }
            }
        }
    }

    cui_column(ui, .width = CUI_FILL, .gap = 3) {
        cui_text(ui, "Resolution & refresh rate", .font = CUI_FONT_MEDIUM, .size = 15);
        cui_secondary(ui, "Successful changes are restored at the next session.");
    }
    for (i = 0; i < d->mode_count; i++) {
        const DisplayMode *m = &d->modes[i];
        if (!cur || strcmp(m->output, cur->name)) continue;
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
