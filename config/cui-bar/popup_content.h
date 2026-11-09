/* CUI control-center widgets shared in behavior with dwm dashboard. */
typedef struct {
    char selected_wifi[32], selected_bt[32], password[256];
    char display_output[128], wallpaper[256];
    float volume;
    int page, initialized_volume, volume_pending;
} UIState;

static void style(cui_ctx *ui) {
    cui_theme *t = cui_theme_get(ui);
    t->colors[CUI_TOKEN_BACKGROUND] = CUI_RGB(0x10131a);
    t->colors[CUI_TOKEN_SURFACE] = CUI_RGB(0x191e27);
    t->colors[CUI_TOKEN_SURFACE_RAISED] = CUI_RGB(0x222936);
    t->colors[CUI_TOKEN_SURFACE_MUTED] = CUI_RGB(0x191e27);
    t->colors[CUI_TOKEN_FOREGROUND] = CUI_RGB(0xe5e9f0);
    t->colors[CUI_TOKEN_TEXT_SECONDARY] = CUI_RGB(0xb6c0cf);
    t->colors[CUI_TOKEN_TEXT_MUTED] = CUI_RGB(0x919eaf);
    t->colors[CUI_TOKEN_BORDER] = CUI_RGB(0x2a323e);
    t->colors[CUI_TOKEN_BORDER_SUBTLE] = CUI_RGB(0x2a323e);
    t->colors[CUI_TOKEN_BORDER_STRONG] = CUI_RGB(0x3a4655);
    t->colors[CUI_TOKEN_ACCENT] = CUI_RGB(0xf1f4f8);
    t->colors[CUI_TOKEN_ACCENT_STRONG] = CUI_RGB(0xf1f4f8);
    t->colors[CUI_TOKEN_ACCENT_SUBTLE] = CUI_RGB(0x3a4655);
    t->colors[CUI_TOKEN_ACCENT_FOREGROUND] = CUI_RGB(0x10131a);
    t->colors[CUI_TOKEN_CONTROL_ON] = CUI_RGB(0xb8cfe1);
    t->colors[CUI_TOKEN_CONTROL_GLYPH] = CUI_RGB(0x10131a);
    t->colors[CUI_TOKEN_CONTROL_THUMB_ON] = CUI_RGB(0x10131a);
    t->colors[CUI_TOKEN_DANGER] = CUI_RGB(0xd8aeb5);
    t->colors[CUI_TOKEN_INFO] = CUI_RGB(0xb8cfe1);
    t->colors[CUI_TOKEN_HOVER] = CUI_RGB(0x222936);
    t->colors[CUI_TOKEN_FOCUS_RING] = CUI_RGB(0xb8cfe1);
    t->radius_control = 6; t->radius_panel = 8; t->radius_surface = 8;
    t->text_body = 13; t->heading[2] = 18;
}

static void wifi_page(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    int i, chosen = -1;
    bool enabled = s->wifi_enabled;
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .width = CUI_FILL, .gap = 3) {
                cui_label(ui, "Wi-Fi");
                cui_secondary(ui, s->wifi_available ? (enabled ? "NetworkManager · wireless enabled" : "Wireless disabled") : "NetworkManager / nmcli unavailable");
            }
            if (cui_switch(ui, "##wifi", &enabled, .disabled = busy || !s->wifi_available))
                request_action(enabled ? ACT_WIFI_ON : ACT_WIFI_OFF, NULL, NULL, 0);
        }
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, "Scan", .icon = CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM, .disabled = busy || !s->wifi_enabled))
                request_action(ACT_WIFI_SCAN, NULL, NULL, 0);
            if (cui_button(ui, "Disconnect", .size = CUI_SIZE_SM, .disabled = busy || !s->wifi_device[0]))
                request_action(ACT_WIFI_DISCONNECT, s->wifi_device, NULL, 0);
            if (cui_button(ui, "Network settings", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) open_settings("nm-connection-editor");
        }
    }
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
        cui_label(ui, "Nearby networks");
        if (!s->networks_count) cui_secondary(ui, s->wifi_enabled ? "No networks found. Try Scan." : "Turn on Wi-Fi to browse networks.");
        for (i = 0; i < s->networks_count; i++) {
            const Network *n = &s->networks[i];
            if (!strcmp(state->selected_wifi, n->mac)) chosen = i;
            cui_id_scope(ui, n->mac) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                    cui_column(ui, .width = CUI_FILL, .gap = 2) {
                        cui_text(ui, n->ssid, .font = CUI_FONT_MEDIUM);
                        cui_textf(ui, "%d%%  ·  %s%s", n->signal,
                                  n->security[0] && strcmp(n->security, "--") ? n->security : "Open",
                                  n->active ? "  ·  Connected" : "");
                    }
                    if (cui_button(ui, !strcmp(state->selected_wifi, n->mac) ? "Selected" : "Select", .size = CUI_SIZE_SM, .selected = !strcmp(state->selected_wifi, n->mac), .disabled = busy)) {
                        if (strcmp(state->selected_wifi, n->mac)) wipe(state->password, sizeof state->password);
                        copystr(state->selected_wifi, sizeof state->selected_wifi, n->mac);
                    }
                }
            }
        }
    }
    if (chosen >= 0) {
        const Network *n = &s->networks[chosen];
        bool secure = n->security[0] && strcmp(n->security, "--");
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_label(ui, n->ssid);
            if (secure) {
                cui_secondary(ui, "Use a saved connection, or enter its Wi-Fi password.");
                cui_input(ui, "wifi-password", state->password, sizeof state->password,
                          .label = "Password", .password = true, .placeholder = "Optional for saved networks", .disabled = busy);
            } else cui_secondary(ui, "Open network · no password required.");
            cui_row(ui, .gap = 8, .wrap = true) {
                if (cui_button(ui, "Connect", .variant = CUI_BUTTON_PRIMARY, .disabled = busy || n->active)) {
                    if (!strchr(state->password, '\n') && !strchr(state->password, '\r') &&
                        request_action(ACT_WIFI_CONNECT, n->mac, secure ? state->password : NULL, 0))
                        wipe(state->password, sizeof state->password);
                }
                if (cui_button(ui, "Clear selection", .variant = CUI_BUTTON_GHOST, .disabled = busy)) {
                    state->selected_wifi[0] = '\0'; wipe(state->password, sizeof state->password);
                }
            }
            if (secure) cui_secondary(ui, "WPA personal password via private pipe. For enterprise networks, use Network settings.");
        }
    }
}

static void bluetooth_page(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    int i, chosen = -1;
    bool enabled = s->bluetooth_enabled;
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .width = CUI_FILL, .gap = 3) {
                cui_label(ui, "Bluetooth");
                cui_secondary(ui, s->bluetooth_available ? (enabled ? "BlueZ · controller powered on" : "Controller powered off") : "No controller or bluetoothctl unavailable");
            }
            if (cui_switch(ui, "##bluetooth", &enabled, .disabled = busy || !s->bluetooth_available))
                request_action(enabled ? ACT_BT_ON : ACT_BT_OFF, NULL, NULL, 0);
        }
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, "Scan for 8 seconds", .icon = CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM, .disabled = busy || !enabled))
                request_action(ACT_BT_SCAN, NULL, NULL, 0);
            if (cui_button(ui, "Bluetooth settings", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) open_settings("blueman-manager");
        }
    }
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
        cui_label(ui, "Devices");
        if (!s->devices_count) cui_secondary(ui, enabled ? "No devices yet. Make your device discoverable and scan." : "Turn on Bluetooth to browse devices.");
        for (i = 0; i < s->devices_count; i++) {
            const Device *d = &s->devices[i];
            if (!strcmp(state->selected_bt, d->mac)) chosen = i;
            cui_id_scope(ui, d->mac) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                    cui_column(ui, .width = CUI_FILL, .gap = 2) {
                        cui_text(ui, d->name, .font = CUI_FONT_MEDIUM);
                        cui_textf(ui, "%s  ·  %s", d->mac,
                                  !s->connected_known ? "Status unknown" : d->connected ? "Connected" :
                                  !s->paired_known ? "Not connected" : d->paired ? "Paired" : "Not paired");
                    }
                    if (cui_button(ui, !strcmp(state->selected_bt, d->mac) ? "Selected" : "Select", .size = CUI_SIZE_SM, .selected = !strcmp(state->selected_bt, d->mac), .disabled = busy))
                        copystr(state->selected_bt, sizeof state->selected_bt, d->mac);
                }
            }
        }
    }
    if (chosen >= 0) {
        const Device *d = &s->devices[chosen];
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_label(ui, d->name);
            cui_secondary(ui, "Pairing may require an active system Bluetooth agent and confirmation.");
            cui_row(ui, .gap = 8, .wrap = true) {
                if (cui_button(ui, "Pair", .disabled = busy || !enabled || (s->paired_known && d->paired)))
                    request_action(ACT_BT_PAIR, d->mac, NULL, 0);
                if (cui_button(ui, "Connect", .variant = CUI_BUTTON_PRIMARY, .disabled = busy || !enabled || (s->connected_known && d->connected)))
                    request_action(ACT_BT_CONNECT, d->mac, NULL, 0);
                if (cui_button(ui, "Disconnect", .disabled = busy || !enabled || (s->connected_known && !d->connected)))
                    request_action(ACT_BT_DISCONNECT, d->mac, NULL, 0);
            }
        }
    }
}

static void audio_page(cui_ctx *ui, UIState *state, const Snapshot *s, bool busy) {
    if ((!busy && !state->volume_pending && !cui_mouse_down(ui, CUI_MOUSE_LEFT)) || !state->initialized_volume) {
        state->volume = (float)s->volume;
        if (state->volume > 100) state->volume = 100;
        state->initialized_volume = 1;
    }
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 14) {
        cui_label(ui, "Output volume");
        cui_secondary(ui, s->audio_available ? (s->muted ? "Default output is muted" : "Default output") : "PulseAudio-compatible server / pactl unavailable");
        if (cui_slider(ui, "Volume", &state->volume, 0, 100, .step = 1, .show_value = true, .format = "%.0f%%", .width = CUI_FILL, .disabled = !s->audio_available))
            state->volume_pending = 1;
        if (state->volume_pending && !busy && !cui_mouse_down(ui, CUI_MOUSE_LEFT) &&
            request_action(ACT_AUDIO_VOLUME, NULL, NULL, (int)state->volume))
            state->volume_pending = 0;
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, s->muted ? "Unmute" : "Mute", .icon = s->muted ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, .disabled = busy || !s->audio_available))
                request_action(ACT_AUDIO_MUTE, NULL, NULL, 0);
            if (cui_button(ui, "Sound settings", .variant = CUI_BUTTON_GHOST)) open_settings("pavucontrol");
        }
    }
}

#include "../dwm/dashboard/desktop_pages.h"
