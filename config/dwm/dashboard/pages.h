/* CUI control-center widgets shared in behavior with dwm dashboard. */
typedef struct {
    char selected_wifi[32], selected_bt[32], password[256];
    char display_output[128], display_target[128];
    float volume;
    int page, initialized_volume, volume_pending, display_layout, display_scale;
} UIState;

#include "theme.h"

/* A fixed sidebar on roomy windows, compact icon navigation on small screens. */
static void dashboard_nav(cui_ctx *ui, UIState *state, bool compact) {
    static const char *names[] = {"Overview", "Wi-Fi", "Bluetooth", "Sound", "Display"};
    static const char *icons[] = {CUI_ICON_LAYOUT_DASHBOARD, CUI_ICON_WIFI, CUI_ICON_LINK, CUI_ICON_VOLUME_2, CUI_ICON_MONITOR};
    static const int page[] = {4, 0, 1, 2, 3};
    int i;
    cui_box(ui, .dir = compact ? CUI_ROW : CUI_COLUMN, .width = CUI_FILL, .gap = compact ? 3 : 6) {
        for (i = 0; i < 5; i++) {
            cui_id_scope(ui, names[i]) {
                if (cui_button(ui, compact ? "" : names[i], .icon = icons[i], .size = CUI_SIZE_SM,
                               .tooltip = names[i], .width = CUI_FILL, .selected = state->page == page[i])) state->page = page[i];
            }
        }
    }
}

static void volume_flush(cui_ctx *ui, UIState *state, bool busy) {
    if (state->volume_pending && !busy && !cui_mouse_down(ui, CUI_MOUSE_LEFT) &&
        request_action(ACT_AUDIO_VOLUME, NULL, NULL, (int)state->volume))
        state->volume_pending = 0;
}
static void volume_control(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    if ((!busy && !state->volume_pending && !cui_mouse_down(ui, CUI_MOUSE_LEFT)) || !state->initialized_volume) {
        state->volume = (float)s->volume;
        if (state->volume > 100) state->volume = 100;
        state->initialized_volume = 1;
    }
    if (cui_slider(ui, "Volume", &state->volume, 0, 100, .step = 1, .show_value = true,
                   .format = "%.0f%%", .width = CUI_FILL, .disabled = !s->audio_available))
        state->volume_pending = 1;
    volume_flush(ui, state, busy);
}

/* Overview provides quick actions without making individual settings pages
 * inaccessible; its controls use exactly the same asynchronous backend. */
static void dashboard_home(cui_ctx *ui, UIState *state, const Snapshot *s, const DesktopState *d, bool busy) {
    const char *ssid = NULL; int i, connected = 0; bool wifi = s->wifi_enabled, bt = s->bluetooth_enabled;
    for (i = 0; i < s->networks_count; i++) if (s->networks[i].active) { ssid = s->networks[i].ssid; break; }
    for (i = 0; i < s->devices_count; i++) if (s->devices[i].connected) connected++;
    cui_column(ui, .width = CUI_FILL, .gap = 5) {
        cui_text(ui, "YOUR SESSION", .size = 10, .color = CUI_COLOR_INFO);
        cui_text(ui, "Overview", .font = CUI_FONT_MEDIUM, .size = 22);
        cui_secondary(ui, "Quick access to your everyday controls.");
    }
    /* One prominent, always-visible audio control instead of a grid of tiles. */
    cui_card(ui, .width = CUI_FILL, .pad = 15, .gap = 9) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, CUI_ICON_VOLUME_2, .font = CUI_FONT_ICONS, .size = 18, .color = CUI_COLOR_INFO);
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, "Sound output", .font = CUI_FONT_MEDIUM, .size = 15);
                cui_secondary(ui, s->audio_available ? (s->muted ? "Muted" : "Default output") : "Audio unavailable");
            }
            if (cui_button(ui, s->muted ? "Unmute" : "Mute", .size = CUI_SIZE_SM, .disabled = busy || !s->audio_available))
                request_action(ACT_AUDIO_MUTE, NULL, NULL, 0);
        }
        volume_control(ui, state, s, busy);
        if (cui_button(ui, "Sound settings", .icon_right = CUI_ICON_ARROW_RIGHT, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) state->page = 2;
    }
    cui_column(ui, .width = CUI_FILL, .gap = 4) {
        cui_text(ui, "CONNECTIONS", .size = 10, .color = CUI_COLOR_TEXT_SECONDARY);
    }
    cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 10) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, CUI_ICON_WIFI, .font = CUI_FONT_ICONS, .size = 18, .color = CUI_COLOR_INFO);
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, "Wi-Fi", .font = CUI_FONT_MEDIUM, .size = 14);
                cui_text(ui, !s->wifi_available ? "Unavailable" : !wifi ? "Off" : ssid && s->wifi_device[0] ? ssid : s->wifi_device[0] ? "Connected" : "Not connected", .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            if (cui_switch(ui, "##home-wifi", &wifi, .disabled = busy || !s->wifi_available))
                request_action(wifi ? ACT_WIFI_ON : ACT_WIFI_OFF, NULL, NULL, 0);
            cui_id_scope(ui, "home-networks") {
                if (cui_button(ui, "", .icon = CUI_ICON_ARROW_RIGHT, .tooltip = "Networks", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) state->page = 0;
            }
        }
        cui_separator(ui);
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, CUI_ICON_LINK, .font = CUI_FONT_ICONS, .size = 18, .color = CUI_COLOR_INFO);
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, "Bluetooth", .font = CUI_FONT_MEDIUM, .size = 14);
                if (!s->bluetooth_available) cui_secondary(ui, "Unavailable");
                else if (!bt) cui_secondary(ui, "Off");
                else if (s->connected_known) cui_textf(ui, "%d connected", connected);
                else cui_secondary(ui, "Status unavailable");
            }
            if (cui_switch(ui, "##home-bt", &bt, .disabled = busy || !s->bluetooth_available))
                request_action(bt ? ACT_BT_ON : ACT_BT_OFF, NULL, NULL, 0);
            cui_id_scope(ui, "home-devices") {
                if (cui_button(ui, "", .icon = CUI_ICON_ARROW_RIGHT, .tooltip = "Devices", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) state->page = 1;
            }
        }
    }
    cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 8) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, CUI_ICON_MONITOR, .font = CUI_FONT_ICONS, .size = 18, .color = CUI_COLOR_INFO);
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, "Displays", .font = CUI_FONT_MEDIUM, .size = 14);
                if (d->xrandr_available) cui_textf(ui, "%d connected", d->output_count);
                else cui_secondary(ui, "Display service unavailable");
            }
            if (cui_button(ui, "Manage", .size = CUI_SIZE_SM, .icon_right = CUI_ICON_ARROW_RIGHT)) state->page = 3;
        }
    }
}

/* Each device is its own card, with actions next to the selected item. */
static void wifi_page(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    int i; bool enabled = s->wifi_enabled;
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 12) {
            cui_column(ui, .width = CUI_FILL, .gap = 4) {
                cui_text(ui, "Wireless", .font = CUI_FONT_MEDIUM, .size = 16);
                cui_secondary(ui, s->wifi_available ? (enabled ? "Wi-Fi is on · Manage your connection" : "Wi-Fi is off") : "NetworkManager is unavailable");
            }
            if (cui_switch(ui, "##wifi", &enabled, .disabled = busy || !s->wifi_available))
                request_action(enabled ? ACT_WIFI_ON : ACT_WIFI_OFF, NULL, NULL, 0);
        }
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, "Scan", .icon = CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM, .disabled = busy || !s->wifi_enabled))
                request_action(ACT_WIFI_SCAN, NULL, NULL, 0);
            if (cui_button(ui, "Disconnect", .size = CUI_SIZE_SM, .disabled = busy || !s->wifi_device[0]))
                request_action(ACT_WIFI_DISCONNECT, s->wifi_device, NULL, 0);
            if (cui_button(ui, "Advanced", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) open_settings("nm-connection-editor");
        }
    }
    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
        cui_column(ui, .width = CUI_FILL, .gap = 3) {
            cui_text(ui, "Available networks", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_secondary(ui, "Select a network to see connection options.");
        }
        cui_textf(ui, "%d found", s->networks_count);
    }
    if (!s->networks_count) cui_card(ui, .width = CUI_FILL, .pad = 16) {
        cui_secondary(ui, s->wifi_enabled ? "No networks found. Try scanning again." : "Turn on Wi-Fi to find nearby networks.");
    }
    for (i = 0; i < s->networks_count; i++) {
        const Network *n = &s->networks[i]; bool selected = !strcmp(state->selected_wifi, n->mac);
        bool secure = n->security[0] && strcmp(n->security, "--");
        cui_id_scope(ui, n->mac) {
            cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 10) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10) {
                    cui_column(ui, .width = CUI_FILL, .gap = 4) {
                        cui_text(ui, n->ssid, .font = CUI_FONT_MEDIUM, .size = 14);
                        cui_textf(ui, "%s  ·  %d%% signal  ·  %s", n->active ? "Connected" : "Available", n->signal, secure ? n->security : "Open");
                    }
                    if (cui_button(ui, selected ? "Close" : "Details", .size = CUI_SIZE_SM, .selected = selected, .disabled = busy)) {
                        if (selected) state->selected_wifi[0] = 0;
                        else copystr(state->selected_wifi, sizeof state->selected_wifi, n->mac);
                        wipe(state->password, sizeof state->password);
                    }
                }
                if (selected) {
                    cui_separator(ui);
                    if (secure && !n->active) {
                        cui_input(ui, "wifi-password", state->password, sizeof state->password,
                                  .label = "Password", .password = true, .placeholder = "Optional for saved networks", .disabled = busy);
                        cui_secondary(ui, "WPA personal · For enterprise networks, use Advanced.");
                    } else if (!secure) cui_secondary(ui, "Open network · no password required.");
                    if (n->active) {
                        cui_secondary(ui, "You are connected to this network.");
                        if (cui_button(ui, "Disconnect", .size = CUI_SIZE_SM, .disabled = busy || !s->wifi_device[0]))
                            request_action(ACT_WIFI_DISCONNECT, s->wifi_device, NULL, 0);
                    }
                    if (!n->active && cui_button(ui, "Connect", .variant = CUI_BUTTON_PRIMARY, .disabled = busy)) {
                        if (!strchr(state->password, '\n') && !strchr(state->password, '\r') &&
                            request_action(ACT_WIFI_CONNECT, n->mac, secure ? state->password : NULL, 0))
                            wipe(state->password, sizeof state->password);
                    }
                }
            }
        }
    }
}

static void bluetooth_page(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    int i; bool enabled = s->bluetooth_enabled;
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 12) {
            cui_column(ui, .width = CUI_FILL, .gap = 4) {
                cui_text(ui, "Bluetooth", .font = CUI_FONT_MEDIUM, .size = 16);
                cui_secondary(ui, s->bluetooth_available ? (enabled ? "On · Ready to connect" : "Bluetooth is off") : "Controller or BlueZ unavailable");
            }
            if (cui_switch(ui, "##bluetooth", &enabled, .disabled = busy || !s->bluetooth_available))
                request_action(enabled ? ACT_BT_ON : ACT_BT_OFF, NULL, NULL, 0);
        }
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, "Scan for devices", .icon = CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM, .disabled = busy || !enabled))
                request_action(ACT_BT_SCAN, NULL, NULL, 0);
            if (cui_button(ui, "Advanced", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) open_settings("blueman-manager");
        }
    }
    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
        cui_column(ui, .width = CUI_FILL, .gap = 3) {
            cui_text(ui, "Your devices", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_secondary(ui, "Select a device to manage it.");
        }
        cui_textf(ui, "%d found", s->devices_count);
    }
    if (!s->devices_count) cui_card(ui, .width = CUI_FILL, .pad = 16) {
        cui_secondary(ui, enabled ? "No devices found. Make your device discoverable and scan." : "Turn on Bluetooth to browse devices.");
    }
    for (i = 0; i < s->devices_count; i++) {
        const Device *d = &s->devices[i]; bool selected = !strcmp(state->selected_bt, d->mac);
        cui_id_scope(ui, d->mac) {
            cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 10) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10) {
                    cui_column(ui, .width = CUI_FILL, .gap = 4) {
                        cui_text(ui, d->name, .font = CUI_FONT_MEDIUM, .size = 14);
                        cui_textf(ui, "%s  ·  %s", !s->connected_known ? "Status unknown" : d->connected ? "Connected" :
                                  !s->paired_known ? "Not connected" : d->paired ? "Paired" : "Not paired", d->mac);
                    }
                    if (cui_button(ui, selected ? "Close" : "Details", .size = CUI_SIZE_SM, .selected = selected, .disabled = busy)) {
                        if (selected) state->selected_bt[0] = 0;
                        else copystr(state->selected_bt, sizeof state->selected_bt, d->mac);
                    }
                }
                if (selected) {
                    cui_separator(ui);
                    cui_row(ui, .gap = 8, .wrap = true) {
                        if (cui_button(ui, "Pair", .size = CUI_SIZE_SM, .disabled = busy || !enabled || (s->paired_known && d->paired)))
                            request_action(ACT_BT_PAIR, d->mac, NULL, 0);
                        if (cui_button(ui, "Connect", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_PRIMARY, .disabled = busy || !enabled || (s->connected_known && d->connected)))
                            request_action(ACT_BT_CONNECT, d->mac, NULL, 0);
                        if (cui_button(ui, "Disconnect", .size = CUI_SIZE_SM, .disabled = busy || !enabled || (s->connected_known && !d->connected)))
                            request_action(ACT_BT_DISCONNECT, d->mac, NULL, 0);
                    }
                    cui_secondary(ui, "Pairing may require a Bluetooth agent and confirmation.");
                }
            }
        }
    }
}

static void audio_page(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    cui_column(ui, .width = CUI_FILL, .gap = 4) {
        cui_text(ui, "Sound", .font = CUI_FONT_MEDIUM, .size = 20);
        cui_secondary(ui, "Manage your default output");
    }
    cui_card(ui, .width = CUI_FILL, .pad = 18, .gap = 14) {
        cui_text(ui, "Output volume", .font = CUI_FONT_MEDIUM, .size = 16);
        cui_secondary(ui, s->audio_available ? (s->muted ? "Default output is muted" : "Default output is active") : "PulseAudio-compatible server / pactl unavailable");
        volume_control(ui, state, s, busy);
        cui_separator(ui);
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, s->muted ? "Unmute" : "Mute", .icon = s->muted ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, .disabled = busy || !s->audio_available))
                request_action(ACT_AUDIO_MUTE, NULL, NULL, 0);
            if (cui_button(ui, "Sound settings", .variant = CUI_BUTTON_GHOST)) open_settings("pavucontrol");
        }
    }
}

#include "desktop_pages.h"

/* Shared shell keeps the bar popup and the optional CUI dashboard identical. */
static bool dashboard_shell(cui_ctx *ui, UIState *state, const Snapshot *s, const DesktopState *d, bool busy) {
    static const char *scroll_ids[] = {"controls-wifi", "controls-bluetooth", "controls-sound", "controls-display", "controls-home"};
    bool close = false, compact = cui_viewport_size(ui).x < 450;
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 14, .gap = 10) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10) {
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                cui_text(ui, "CONTROL CENTER", .font = CUI_FONT_MEDIUM, .size = 13);
                cui_text(ui, "Your desktop, in one place", .size = 10, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            if (cui_button(ui, "Close", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) close = true;
        }
        volume_flush(ui, state, busy);
        if (compact) dashboard_nav(ui, state, true);
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 12) {
            if (!compact) {
                cui_column(ui, .width = CUI_PX(124), .height = CUI_FILL, .pad = 8, .gap = 10,
                           .bg = CUI_RGB(0x121417), .border = 1, .border_color = CUI_RGB(0x1e2025), .radius = 10) {
                    cui_text(ui, "SECTIONS", .size = 10, .color = CUI_COLOR_TEXT_SECONDARY);
                    dashboard_nav(ui, state, false);
                    cui_spacer(ui);
                    cui_text(ui, "DWM / SESSION", .size = 9, .color = CUI_COLOR_TEXT_SECONDARY);
                }
            }
            cui_scroll(ui, .id = scroll_ids[state->page], .width = CUI_FILL, .height = CUI_FILL, .gap = 12) {
                if (busy) cui_text(ui, "Working...", .color = CUI_COLOR_INFO);
                else if (s->message[0]) cui_text(ui, s->message, .color = CUI_COLOR_TEXT_SECONDARY);
                if (state->page == 0) wifi_page(ui, state, s, busy);
                else if (state->page == 1) bluetooth_page(ui, state, s, busy);
                else if (state->page == 2) audio_page(ui, state, s, busy);
                else if (state->page == 3) display_page(ui, state, d, busy);
                else dashboard_home(ui, state, s, d, busy);
            }
        }
    }
    return close;
}
