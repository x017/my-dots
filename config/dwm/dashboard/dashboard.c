/* CUI-based X11 control center for NetworkManager, BlueZ and PulseAudio. */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_icons.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>

static bool dashboard_worker_stopping(void);
#define DASHBOARD_STOPPING() dashboard_worker_stopping()
#include "../dashboard_backend.h"
#include "desktop_controls.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t thread;
    int stopping, busy, pending;
    Request request;
    Snapshot state;
    DesktopState desktop;
} Backend;

static Backend backend = { .mutex = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static bool dashboard_worker_stopping(void) {
    bool stop;
    pthread_mutex_lock(&backend.mutex); stop = backend.stopping; pthread_mutex_unlock(&backend.mutex);
    return stop;
}

static void *worker(void *unused) {
    Snapshot snapshot;
    DesktopState desktop;
    Request request;
    char message[256] = "";
    (void)unused;
    while (!dashboard_worker_stopping()) {
        int has_request = 0;
        struct timespec until;
        pthread_mutex_lock(&backend.mutex);
        if (backend.pending) {
            request = backend.request; wipe(&backend.request, sizeof backend.request);
            backend.pending = 0; has_request = 1;
        }
        pthread_mutex_unlock(&backend.mutex);
        if (has_request) {
            int rc;
            message[0] = 0;
            if (request.type == ACT_DISPLAY_APPLY || request.type == ACT_WALLPAPER_APPLY)
                rc = desktop_execute(&request, message, sizeof message);
            else rc = execute(&request);
            if (rc == 0 && !message[0]) copystr(message, sizeof message, "Done.");
            else if (request.type == ACT_WIFI_CONNECT) copystr(message, sizeof message, "Connection failed. Check password, permissions or NetworkManager.");
            else if (request.type == ACT_BT_PAIR) copystr(message, sizeof message, "Pairing failed. You may need a Bluetooth pairing agent; open Bluetooth settings.");
            else if (rc && !message[0]) copystr(message, sizeof message, "Action failed. Check device availability and permissions.");
            wipe(&request, sizeof request);
        }
        if (dashboard_worker_stopping()) break;
        read_snapshot(&snapshot);
        desktop_read(&desktop);
        copystr(snapshot.message, sizeof snapshot.message, message);
        pthread_mutex_lock(&backend.mutex);
        backend.state = snapshot; backend.desktop = desktop;
        if (has_request) backend.busy = 0;
        clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 3;
        if (!backend.pending && !backend.stopping) pthread_cond_timedwait(&backend.wake, &backend.mutex, &until);
        pthread_mutex_unlock(&backend.mutex);
    }
    return NULL;
}

static bool request_action(Action action, const char *arg, const char *password, int value) {
    bool queued = false;
    pthread_mutex_lock(&backend.mutex);
    if (!backend.busy && !backend.pending) {
        memset(&backend.request, 0, sizeof backend.request);
        backend.request.type = action; backend.request.value = value;
        copystr(backend.request.arg, sizeof backend.request.arg, arg);
        copystr(backend.request.password, sizeof backend.request.password, password);
        backend.pending = backend.busy = 1; queued = true;
        pthread_cond_signal(&backend.wake);
    }
    pthread_mutex_unlock(&backend.mutex);
    return queued;
}

static void open_settings(const char *program) {
    pid_t p = fork();
    if (p == 0) {
        pid_t child = fork();
        if (child == 0) { setsid(); execlp(program, program, (char *)NULL); _exit(127); }
        _exit(child < 0 ? 1 : 0);
    }
    if (p > 0) { int status; while (waitpid(p, &status, 0) < 0 && errno == EINTR) ; }
}

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

#include "desktop_pages.h"

#ifdef DWM_EMBEDDED_CUI
int dashboard_cui_main(void) {
#else
int main(void) {
#endif
    cui_app *app;
    cui_ctx *ui;
    UIState state = {0};
    static const char *pages[] = {"Wi-Fi", "Bluetooth", "Sound", "Display", "Wallpaper"};
    signal(SIGPIPE, SIG_IGN);
    app = cui_app_create(&(cui_app_desc){ .title = "Dwm Control Center", .width = 760, .height = 700,
                                         .min_width = 480, .min_height = 400, .theme = CUI_THEME_DARK });
    if (!app) { fprintf(stderr, "dwm-dashboard: could not open an X11 window\n"); return 1; }
    ui = cui_app_ui(app);
    style(ui); load_monospace(ui);
    if (pthread_create(&backend.thread, NULL, worker, NULL) != 0) {
        fprintf(stderr, "dwm-dashboard: unable to start worker\n"); cui_app_destroy(app); return 1;
    }
    while (cui_app_frame(app)) {
        Snapshot snapshot; DesktopState desktop; bool busy;
        pthread_mutex_lock(&backend.mutex);
        snapshot = backend.state; desktop = backend.desktop; busy = backend.busy;
        pthread_mutex_unlock(&backend.mutex);
        cui_request_wakeup(ui, 0.25f);
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) cui_app_quit(app);
        cui_scroll(ui, .id = "main", .width = CUI_FILL, .height = CUI_FILL, .pad = 22, .gap = 16) {
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_column(ui, .width = CUI_FILL, .gap = 3) {
                    cui_text(ui, "C O N T R O L   C E N T E R", .font = CUI_FONT_MEDIUM, .size = 16);
                    cui_text(ui, "WIRELESS  /  AUDIO  /  DESKTOP", .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                }
                if (cui_button(ui, "Close", .variant = CUI_BUTTON_GHOST)) cui_app_quit(app);
            }
            cui_separator(ui);
            cui_segmented(ui, "pages", pages, 5, &state.page, .full_width = true);
            if (busy) cui_text(ui, "Working…", .color = CUI_COLOR_INFO);
            else if (snapshot.message[0]) cui_text(ui, snapshot.message, .color = CUI_COLOR_TEXT_SECONDARY);
            if (state.page == 0) wifi_page(ui, &state, &snapshot, busy);
            else if (state.page == 1) bluetooth_page(ui, &state, &snapshot, busy);
            else if (state.page == 2) audio_page(ui, &state, &snapshot, busy);
            else if (state.page == 3) display_page(ui, &state, &desktop, busy);
            else wallpaper_page(ui, &state, &desktop, busy);
        }
    }
    pthread_mutex_lock(&backend.mutex);
    backend.stopping = 1; pthread_cond_signal(&backend.wake);
    pthread_mutex_unlock(&backend.mutex);
    pthread_join(backend.thread, NULL);
    wipe(state.password, sizeof state.password);
    wipe(&backend.request, sizeof backend.request);
    cui_app_destroy(app);
    return 0;
}
