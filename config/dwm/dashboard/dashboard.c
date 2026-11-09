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

#define MAX_ITEMS 48
#define MAX_OUTPUT 32768

typedef struct { char ssid[128], mac[32], security[96]; int signal, active; } Network;
typedef struct { char name[160], mac[32]; int paired, connected; } Device;
typedef struct {
    int wifi_available, wifi_enabled, bluetooth_available, bluetooth_enabled, audio_available;
    int muted, volume, networks_count, devices_count, paired_known, connected_known;
    char wifi_device[64];
    Network networks[MAX_ITEMS];
    Device devices[MAX_ITEMS];
    char message[256];
} Snapshot;
typedef enum { ACT_NONE, ACT_WIFI_ON, ACT_WIFI_OFF, ACT_WIFI_SCAN, ACT_WIFI_CONNECT,
               ACT_WIFI_DISCONNECT, ACT_BT_ON, ACT_BT_OFF, ACT_BT_SCAN, ACT_BT_PAIR,
               ACT_BT_CONNECT, ACT_BT_DISCONNECT, ACT_AUDIO_MUTE, ACT_AUDIO_VOLUME } Action;
typedef struct { Action type; char arg[128], password[256]; int value; } Request;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t thread;
    int stopping, busy, pending;
    Request request;
    Snapshot state;
} Backend;

static Backend backend = { .mutex = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static void wipe(void *buffer, size_t n) {
    volatile unsigned char *p = buffer;
    while (n--) *p++ = 0;
}
static void copystr(char *dst, size_t capacity, const char *src) {
    if (!capacity) return;
    snprintf(dst, capacity, "%s", src ? src : "");
}
static bool stopping(void) {
    bool stop;
    pthread_mutex_lock(&backend.mutex); stop = backend.stopping; pthread_mutex_unlock(&backend.mutex);
    return stop;
}

/* No shell: names and addresses cannot become commands. Read output with a
 * timeout, and send secrets only through a pipe, never in argv or a file. */
static int run_command(char *const argv[], const char *input, char *out, size_t cap, int timeout_ms) {
    int result = -1, fds[2], in[2], status = 0, flags, done = 0;
    size_t used = 0;
    pid_t pid, waited;
    struct timespec start, now;
    if (cap) out[0] = '\0';
    if (pipe(fds) < 0) return -1;
    if (pipe(in) < 0) { close(fds[0]); close(fds[1]); return -1; }
    pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        dup2(in[0], STDIN_FILENO); dup2(fds[1], STDOUT_FILENO); dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]); close(in[0]); close(in[1]);
        execvp(argv[0], argv); _exit(127);
    }
    close(fds[1]); close(in[0]);
    if (pid < 0) { close(fds[0]); close(in[1]); return -1; }
    if (input && *input) {
        size_t n = strlen(input), written = 0;
        while (written < n) {
            ssize_t w = write(in[1], input + written, n - written);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            written += (size_t)w;
        }
    }
    close(in[1]);
    flags = fcntl(fds[0], F_GETFL, 0); if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!done) {
        char buf[2048]; ssize_t n;
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        poll(&pfd, 1, 100);
        while ((n = read(fds[0], buf, sizeof buf)) > 0) {
            size_t take = cap && used < cap - 1 ? (size_t)n : 0;
            if (take > cap - 1 - used) take = cap - 1 - used;
            if (take) { memcpy(out + used, buf, take); used += take; out[used] = '\0'; }
        }
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) { done = 1; result = WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
        else if (waited < 0 && errno != EINTR) done = 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!done && (stopping() || (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= timeout_ms)) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
            done = 1;
        }
    }
    /* The child can exit between the last read and waitpid. Drain bytes it
     * wrote before exit so short-lived commands do not lose their output. */
    for (;;) {
        char buf[2048];
        ssize_t n = read(fds[0], buf, sizeof buf);
        size_t take;
        if (n <= 0) break;
        take = cap && used < cap - 1 ? (size_t)n : 0;
        if (take > cap - 1 - used) take = cap - 1 - used;
        if (take) { memcpy(out + used, buf, take); used += take; out[used] = '\0'; }
    }
    close(fds[0]);
    return result;
}

/* nmcli -t escapes colons and backslashes; decode each field in place. */
static int fields(char *line, char **parts, int max) {
    char *src = line, *dst = line;
    int n = 1;
    parts[0] = dst;
    while (*src) {
        if (*src == '\\' && src[1]) { src++; *dst++ = *src++; }
        else if (*src == ':' && n < max) { src++; *dst++ = '\0'; parts[n++] = dst; }
        else *dst++ = *src++;
    }
    *dst = '\0';
    return n;
}

static int bluetooth_list(char *output, Device *items, int count, int mark) {
    char *line, *save;
    for (line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char mac[32], name[160]; int i;
        if (sscanf(line, "Device %31s %159[^\n]", mac, name) != 2) continue;
        for (i = 0; i < count && strcmp(items[i].mac, mac); i++) ;
        if (mark == 0 && i == count && count < MAX_ITEMS) {
            copystr(items[i].mac, sizeof items[i].mac, mac);
            copystr(items[i].name, sizeof items[i].name, name); count++;
        }
        if (i < count && mark == 1) items[i].paired = 1;
        if (i < count && mark == 2) items[i].connected = 1;
    }
    return count;
}

static void read_snapshot(Snapshot *s) {
    char buf[MAX_OUTPUT], *line, *save, *part[5];
    char *wifi[] = {"nmcli", "-t", "radio", "wifi", NULL};
    char *networks[] = {"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY,BSSID", "device", "wifi", "list", "--rescan", "no", NULL};
    char *ifaces[] = {"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status", NULL};
    char *btshow[] = {"bluetoothctl", "show", NULL};
    char *btdevices[] = {"bluetoothctl", "devices", NULL};
    char *btpaired[] = {"bluetoothctl", "devices", "Paired", NULL};
    char *btconnected[] = {"bluetoothctl", "devices", "Connected", NULL};
    char *vol[] = {"pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL};
    char *mute[] = {"pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL};
    memset(s, 0, sizeof *s);
    if (run_command(wifi, NULL, buf, sizeof buf, 2500) == 0) {
        s->wifi_available = 1; s->wifi_enabled = !strncmp(buf, "enabled", 7);
        if (s->wifi_enabled && run_command(networks, NULL, buf, sizeof buf, 3500) == 0) {
            for (line = strtok_r(buf, "\n", &save); line && s->networks_count < MAX_ITEMS; line = strtok_r(NULL, "\n", &save)) {
                Network *n;
                if (fields(line, part, 5) != 5 || !part[1][0] || !part[4][0]) continue;
                n = &s->networks[s->networks_count++];
                n->active = part[0][0] == '*'; copystr(n->ssid, sizeof n->ssid, part[1]);
                n->signal = atoi(part[2]); copystr(n->security, sizeof n->security, part[3]);
                copystr(n->mac, sizeof n->mac, part[4]);
            }
        }
        if (run_command(ifaces, NULL, buf, sizeof buf, 2500) == 0) {
            for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
                if (fields(line, part, 3) == 3 && !strcmp(part[1], "wifi") && !strncmp(part[2], "connected", 9)) {
                    copystr(s->wifi_device, sizeof s->wifi_device, part[0]); break;
                }
        }
    }
    if (run_command(btshow, NULL, buf, sizeof buf, 2500) == 0 &&
        strstr(buf, "Powered:")) {
        s->bluetooth_available = 1;
        s->bluetooth_enabled = strstr(buf, "Powered: yes") != NULL;
        if (s->bluetooth_enabled && run_command(btdevices, NULL, buf, sizeof buf, 2500) == 0) {
            s->devices_count = bluetooth_list(buf, s->devices, 0, 0);
            if (run_command(btpaired, NULL, buf, sizeof buf, 2500) == 0) { s->paired_known = 1; bluetooth_list(buf, s->devices, s->devices_count, 1); }
            if (run_command(btconnected, NULL, buf, sizeof buf, 2500) == 0) { s->connected_known = 1; bluetooth_list(buf, s->devices, s->devices_count, 2); }
        }
    }
    if (run_command(vol, NULL, buf, sizeof buf, 2500) == 0) {
        char *percent = strchr(buf, '%');
        if (percent) {
            char *start = percent;
            while (start > buf && isdigit((unsigned char)start[-1])) start--;
            if (start != percent) { s->audio_available = 1; s->volume = atoi(start); }
        }
    }
    if (s->audio_available && run_command(mute, NULL, buf, sizeof buf, 2500) == 0) s->muted = strstr(buf, "yes") != NULL;
}

static int execute(Request *r) {
    char out[MAX_OUTPUT], secret[320], number[32];
    char *wifi_on[] = {"nmcli", "radio", "wifi", "on", NULL};
    char *wifi_off[] = {"nmcli", "radio", "wifi", "off", NULL};
    char *wifi_scan[] = {"nmcli", "device", "wifi", "rescan", NULL};
    char *wifi_connect[] = {"nmcli", "--wait", "20", "device", "wifi", "connect", r->arg, NULL};
    char *wifi_password[] = {"nmcli", "--wait", "20", "--passwd-file", "/dev/stdin", "device", "wifi", "connect", r->arg, NULL};
    char *wifi_disconnect[] = {"nmcli", "device", "disconnect", r->arg, NULL};
    char *bt_on[] = {"bluetoothctl", "power", "on", NULL};
    char *bt_off[] = {"bluetoothctl", "power", "off", NULL};
    char *bt_scan[] = {"bluetoothctl", "--timeout", "8", "scan", "on", NULL};
    char *bt_pair[] = {"bluetoothctl", "--timeout", "20", "pair", r->arg, NULL};
    char *bt_connect[] = {"bluetoothctl", "--timeout", "20", "connect", r->arg, NULL};
    char *bt_disconnect[] = {"bluetoothctl", "--timeout", "10", "disconnect", r->arg, NULL};
    char *audio_mute[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL};
    char *audio_volume[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", number, NULL};
    char **command = NULL; int timeout = 5000, rc;
    secret[0] = '\0';
    switch (r->type) {
    case ACT_WIFI_ON: command = wifi_on; break; case ACT_WIFI_OFF: command = wifi_off; break;
    case ACT_WIFI_SCAN: command = wifi_scan; timeout = 12000; break;
    case ACT_WIFI_CONNECT:
        timeout = 25000;
        if (r->password[0]) { command = wifi_password; snprintf(secret, sizeof secret, "802-11-wireless-security.psk:%s\n", r->password); }
        else command = wifi_connect;
        break;
    case ACT_WIFI_DISCONNECT: command = wifi_disconnect; timeout = 12000; break;
    case ACT_BT_ON: command = bt_on; break; case ACT_BT_OFF: command = bt_off; break;
    case ACT_BT_SCAN: command = bt_scan; timeout = 12000; break;
    case ACT_BT_PAIR: command = bt_pair; timeout = 25000; break;
    case ACT_BT_CONNECT: command = bt_connect; timeout = 25000; break;
    case ACT_BT_DISCONNECT: command = bt_disconnect; timeout = 15000; break;
    case ACT_AUDIO_MUTE: command = audio_mute; break;
    case ACT_AUDIO_VOLUME: snprintf(number, sizeof number, "%d%%", r->value); command = audio_volume; break;
    default: return -1;
    }
    rc = run_command(command, secret[0] ? secret : NULL, out, sizeof out, timeout);
    /* bluetoothctl often exits zero even for a command-level failure. */
    if (command[0] && !strcmp(command[0], "bluetoothctl")) {
        if (strstr(out, "Failed") || strstr(out, "not available") || strstr(out, "No default controller")) rc = 1;
        else if (r->type == ACT_BT_SCAN && strstr(out, "Discovery started")) rc = 0;
    }
    wipe(secret, sizeof secret); wipe(out, sizeof out);
    return rc;
}

static void *worker(void *unused) {
    Snapshot snapshot;
    Request request;
    char message[256] = "";
    (void)unused;
    while (!stopping()) {
        int has_request = 0;
        struct timespec until;
        pthread_mutex_lock(&backend.mutex);
        if (backend.pending) {
            request = backend.request; wipe(&backend.request, sizeof backend.request);
            backend.pending = 0; has_request = 1;
        }
        pthread_mutex_unlock(&backend.mutex);
        if (has_request) {
            int rc = execute(&request);
            if (rc == 0) copystr(message, sizeof message, "Done.");
            else if (request.type == ACT_WIFI_CONNECT) copystr(message, sizeof message, "Connection failed. Check password, permissions or NetworkManager.");
            else if (request.type == ACT_BT_PAIR) copystr(message, sizeof message, "Pairing failed. You may need a Bluetooth pairing agent; open Bluetooth settings.");
            else copystr(message, sizeof message, "Action failed. Check device availability and permissions.");
            wipe(&request, sizeof request);
        }
        if (stopping()) break;
        read_snapshot(&snapshot);
        copystr(snapshot.message, sizeof snapshot.message, message);
        pthread_mutex_lock(&backend.mutex);
        backend.state = snapshot;
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
    float volume;
    int page, initialized_volume;
} UIState;

static void style(cui_ctx *ui) {
    cui_theme *t = cui_theme_get(ui);
    t->colors[CUI_TOKEN_BACKGROUND] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_SURFACE] = CUI_RGB(0x121417);
    t->colors[CUI_TOKEN_SURFACE_RAISED] = CUI_RGB(0x181a1e);
    t->colors[CUI_TOKEN_SURFACE_MUTED] = CUI_RGB(0x121417);
    t->colors[CUI_TOKEN_FOREGROUND] = CUI_RGB(0xadbac7);
    t->colors[CUI_TOKEN_TEXT_SECONDARY] = CUI_RGB(0x8a949e);
    t->colors[CUI_TOKEN_TEXT_MUTED] = CUI_RGB(0x64778d);
    t->colors[CUI_TOKEN_BORDER] = CUI_RGB(0x1e2025);
    t->colors[CUI_TOKEN_BORDER_SUBTLE] = CUI_RGB(0x1e2025);
    t->colors[CUI_TOKEN_BORDER_STRONG] = CUI_RGB(0x2d3035);
    t->colors[CUI_TOKEN_ACCENT] = CUI_RGB(0xf2d49e);
    t->colors[CUI_TOKEN_ACCENT_STRONG] = CUI_RGB(0xf2d49e);
    t->colors[CUI_TOKEN_ACCENT_SUBTLE] = CUI_RGB(0x2d3035);
    t->colors[CUI_TOKEN_ACCENT_FOREGROUND] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_CONTROL_ON] = CUI_RGB(0x7fa8c9);
    t->colors[CUI_TOKEN_CONTROL_GLYPH] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_CONTROL_THUMB_ON] = CUI_RGB(0x090b0d);
    t->colors[CUI_TOKEN_DANGER] = CUI_RGB(0xb86a6a);
    t->colors[CUI_TOKEN_INFO] = CUI_RGB(0x7fa8c9);
    t->colors[CUI_TOKEN_HOVER] = CUI_RGB(0x181a1e);
    t->colors[CUI_TOKEN_FOCUS_RING] = CUI_RGB(0x7fa8c9);
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
    if (!busy || !state->initialized_volume) {
        state->volume = (float)s->volume;
        if (state->volume > 100) state->volume = 100;
        state->initialized_volume = 1;
    }
    cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 14) {
        cui_label(ui, "Output volume");
        cui_secondary(ui, s->audio_available ? (s->muted ? "Default output is muted" : "Default output") : "PulseAudio-compatible server / pactl unavailable");
        if (cui_slider(ui, "Volume", &state->volume, 0, 100, .step = 1, .show_value = true, .format = "%.0f%%", .width = CUI_FILL, .disabled = busy || !s->audio_available))
            request_action(ACT_AUDIO_VOLUME, NULL, NULL, (int)state->volume);
        cui_row(ui, .gap = 8, .wrap = true) {
            if (cui_button(ui, s->muted ? "Unmute" : "Mute", .icon = s->muted ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, .disabled = busy || !s->audio_available))
                request_action(ACT_AUDIO_MUTE, NULL, NULL, 0);
            if (cui_button(ui, "Sound settings", .variant = CUI_BUTTON_GHOST)) open_settings("pavucontrol");
        }
    }
}

int main(void) {
    cui_app *app;
    cui_ctx *ui;
    UIState state = {0};
    static const char *pages[] = {"Wi-Fi", "Bluetooth", "Sound"};
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
        Snapshot snapshot; bool busy;
        pthread_mutex_lock(&backend.mutex);
        snapshot = backend.state; busy = backend.busy;
        pthread_mutex_unlock(&backend.mutex);
        cui_request_wakeup(ui, 0.25f);
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) cui_app_quit(app);
        cui_scroll(ui, .id = "main", .width = CUI_FILL, .height = CUI_FILL, .pad = 22, .gap = 16) {
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_column(ui, .width = CUI_FILL, .gap = 3) {
                    cui_text(ui, "C O N T R O L   C E N T E R", .font = CUI_FONT_MEDIUM, .size = 16);
                    cui_text(ui, "WIRELESS  /  DEVICES  /  AUDIO", .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                }
                if (cui_button(ui, "Close", .variant = CUI_BUTTON_GHOST)) cui_app_quit(app);
            }
            cui_separator(ui);
            cui_segmented(ui, "pages", pages, 3, &state.page, .full_width = true);
            if (busy) cui_text(ui, "Working…", .color = CUI_COLOR_INFO);
            else if (snapshot.message[0]) cui_text(ui, snapshot.message, .color = CUI_COLOR_TEXT_SECONDARY);
            if (state.page == 0) wifi_page(ui, &state, &snapshot, busy);
            else if (state.page == 1) bluetooth_page(ui, &state, &snapshot, busy);
            else audio_page(ui, &state, &snapshot, busy);
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
