/* Standalone CUI/Xlib bar and XEmbed system tray. No dwm IPC or dependencies. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <cui/cui_icons.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#define DASHBOARD_NO_SNAPSHOT
#include "../dwm/dashboard_backend.h"

#define HEIGHT 36
#define ICON_SIZE 24
#define ICON_SLOT 30
#define MAX_ICONS 64
#define PAD 8
#define BUTTON_WIDTH 92
#define CLOCK_WIDTH 160
#define TRAY_PAD 8
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_MAPPED 1

typedef struct { Window win; int mapped; } TrayIcon;
typedef struct {
    Display *dpy;
    int screen, width;
    Window root, win;
    GC gc;
    Atom selection, manager, opcode, orientation, visual, xembed, xembed_info;
    Atom state_atom, command_atom, wm_atom, xkb_rules;
    unsigned long active, occupied, urgent;
    int mx, my, mw, tag_count, worker_fd, worker_busy, worker_completed, worker_waiting;
    Snapshot snapshot;
    int ethernet, network_available, cpu, memory, temperature, battery;
    unsigned long long cpu_total, cpu_idle;
    char clock[32], language[12];
    TrayIcon icons[MAX_ICONS];
    int count, tray_width;
    cui_sw_renderer *renderer;
    cui_ctx *ui;
} Bar;
static volatile sig_atomic_t running = 1;
static void terminate(int sig) { (void)sig; running = 0; }
/* Icons can disappear between receiving their ID and sending X requests. */
static int xerror(Display *display, XErrorEvent *event) {
    if (event->error_code != BadWindow && event->error_code != BadDrawable && event->error_code != BadMatch) {
        char text[128]; XGetErrorText(display, event->error_code, text, sizeof text);
        fprintf(stderr, "cui-bar: X11 error: %s\n", text);
    }
    return 0;
}
static void launch(const char *program) {
    pid_t child = fork();
    if (child == 0) {
        setsid(); execlp(program, program, (char *)NULL);
        fprintf(stderr, "cui-bar: could not start %s: %s\n", program, strerror(errno)); _exit(127);
    }
    if (child < 0) fprintf(stderr, "cui-bar: fork failed: %s\n", strerror(errno));
}
static void xembed(Bar *b, Window icon, long message, long detail, long data1, long data2) {
    XEvent e; memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage; e.xclient.window = icon;
    e.xclient.message_type = b->xembed; e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime; e.xclient.data.l[1] = message;
    e.xclient.data.l[2] = detail; e.xclient.data.l[3] = data1; e.xclient.data.l[4] = data2;
    XSendEvent(b->dpy, icon, False, NoEventMask, &e);
}
static int icon_index(Bar *b, Window w) {
    int i; for (i = 0; i < b->count; i++) if (b->icons[i].win == w) return i;
    return -1;
}
static int icon_mapped(Bar *b, Window w) {
    Atom type; int format; unsigned long count, left; unsigned char *data = NULL;
    int mapped = 1;
    if (XGetWindowProperty(b->dpy, w, b->xembed_info, 0, 2, False, b->xembed_info,
                           &type, &format, &count, &left, &data) == Success &&
        data && type == b->xembed_info && format == 32 && count >= 2)
        mapped = (((unsigned long *)data)[1] & XEMBED_MAPPED) != 0;
    if (data) XFree(data);
    return mapped;
}
static void position_icons(Bar *b) {
    int i, n = 0, x;
    int limit = (b->width - (16 + b->tag_count * 28 + 412)) / ICON_SLOT;
    if (limit < 0) limit = 0;
    if (limit > 8) limit = 8;
    for (i = 0; i < b->count; i++) if (b->icons[i].mapped && n < limit) n++;
    b->tray_width = n ? n * ICON_SLOT + TRAY_PAD : 0;
    x = b->width - CLOCK_WIDTH - b->tray_width + TRAY_PAD / 2;
    n = 0;
    for (i = 0; i < b->count; i++) {
        if (b->icons[i].mapped && n++ < limit) {
            XMoveResizeWindow(b->dpy, b->icons[i].win, x + (ICON_SLOT - ICON_SIZE) / 2,
                              (HEIGHT - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE);
            XMapRaised(b->dpy, b->icons[i].win); x += ICON_SLOT;
        } else if (b->icons[i].mapped) XMoveWindow(b->dpy, b->icons[i].win, -ICON_SIZE, -ICON_SIZE);
        else XUnmapWindow(b->dpy, b->icons[i].win);
    }
}
static void dock(Bar *b, Window icon) {
    XWindowAttributes attr;
    if (!icon || icon == b->win || icon_index(b, icon) >= 0 || b->count >= MAX_ICONS ||
        !XGetWindowAttributes(b->dpy, icon, &attr)) return;
    b->icons[b->count].win = icon;
    b->icons[b->count].mapped = icon_mapped(b, icon);
    b->count++;
    XAddToSaveSet(b->dpy, icon);
    XSelectInput(b->dpy, icon, StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);
    XReparentWindow(b->dpy, icon, b->win, 0, 0);
    xembed(b, icon, XEMBED_EMBEDDED_NOTIFY, 0, (long)b->win, 0);
    xembed(b, icon, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
    position_icons(b);
}
static void remove_icon(Bar *b, int i) {
    if (i < 0 || i >= b->count) return;
    memmove(&b->icons[i], &b->icons[i + 1], (size_t)(b->count - i - 1) * sizeof b->icons[0]);
    b->count--; position_icons(b);
}
static int acquire_tray(Bar *b) {
    char selection[64]; XEvent event;
    unsigned long orientation = 0, visual = XVisualIDFromVisual(DefaultVisual(b->dpy, b->screen));
    snprintf(selection, sizeof selection, "_NET_SYSTEM_TRAY_S%d", b->screen);
    b->selection = XInternAtom(b->dpy, selection, False);
    b->manager = XInternAtom(b->dpy, "MANAGER", False);
    b->opcode = XInternAtom(b->dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    b->orientation = XInternAtom(b->dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    b->visual = XInternAtom(b->dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
    b->xembed = XInternAtom(b->dpy, "_XEMBED", False);
    b->xembed_info = XInternAtom(b->dpy, "_XEMBED_INFO", False);
    XChangeProperty(b->dpy, b->win, b->orientation, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&orientation, 1);
    XChangeProperty(b->dpy, b->win, b->visual, XA_VISUALID, 32, PropModeReplace, (unsigned char *)&visual, 1);
    XGrabServer(b->dpy);
    if (XGetSelectionOwner(b->dpy, b->selection) != None) {
        XUngrabServer(b->dpy); XSync(b->dpy, False);
        fprintf(stderr, "cui-bar: another system tray is already running; stop it first.\n"); return 0;
    }
    XSetSelectionOwner(b->dpy, b->selection, b->win, CurrentTime);
    if (XGetSelectionOwner(b->dpy, b->selection) != b->win) {
        XUngrabServer(b->dpy); XSync(b->dpy, False);
        fprintf(stderr, "cui-bar: could not claim system tray.\n"); return 0;
    }
    XUngrabServer(b->dpy);
    memset(&event, 0, sizeof event);
    event.xclient.type = ClientMessage; event.xclient.window = b->root;
    event.xclient.message_type = b->manager; event.xclient.format = 32;
    event.xclient.data.l[0] = CurrentTime; event.xclient.data.l[1] = b->selection;
    event.xclient.data.l[2] = b->win;
    XSendEvent(b->dpy, b->root, False, StructureNotifyMask, &event);
    XFlush(b->dpy);
    return 1;
}

typedef struct { Snapshot snapshot; int ethernet, network_available, completed; } WorkerFrame;
static int write_all(int fd, const void *data, size_t length) {
    const char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; length -= (size_t)n;
    }
    return 1;
}
static void worker(int fd) {
    WorkerFrame frame = {0}; Request req; char output[MAX_OUTPUT];
    char *devices[] = {"nmcli", "-t", "-f", "TYPE,STATE", "device", "status", NULL};
    char *wifi[] = {"nmcli", "-t", "-f", "IN-USE,SIGNAL", "device", "wifi", "list", "--rescan", "no", NULL};
    char *wifi_radio[] = {"nmcli", "-t", "radio", "wifi", NULL};
    char *bt[] = {"bluetoothctl", "show", NULL};
    char *bt_connected[] = {"bluetoothctl", "devices", "Connected", NULL};
    char *volume[] = {"pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL};
    char *mute[] = {"pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL};
    struct pollfd p = { .fd = fd, .events = POLLIN };
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_IGN);
    setenv("LC_ALL", "C", 1);
    for (;;) {
        int ready; char *percent;
        memset(&frame.snapshot, 0, sizeof frame.snapshot);
        frame.ethernet = frame.network_available = 0;
        if (run_command(devices, NULL, output, sizeof output, 2500) == 0) {
            frame.network_available = 1;
            frame.ethernet = strstr(output, "ethernet:connected") != NULL;
            if (strstr(output, "wifi:connected")) copystr(frame.snapshot.wifi_device, sizeof frame.snapshot.wifi_device, "connected");
        }
        if (run_command(wifi_radio, NULL, output, sizeof output, 2500) == 0) {
            frame.snapshot.wifi_available = 1;
            frame.snapshot.wifi_enabled = !strncmp(output, "enabled", 7);
        }
        if (frame.snapshot.wifi_device[0] && run_command(wifi, NULL, output, sizeof output, 2500) == 0) {
            char *line, *save, *parts[2];
            for (line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
                if (fields(line, parts, 2) == 2 && parts[0][0] == '*') {
                    frame.snapshot.networks_count = 1; frame.snapshot.networks[0].active = 1;
                    frame.snapshot.networks[0].signal = atoi(parts[1]); break;
                }
        }
        if (run_command(bt, NULL, output, sizeof output, 2500) == 0 && strstr(output, "Powered:")) {
            frame.snapshot.bluetooth_available = 1;
            frame.snapshot.bluetooth_enabled = strstr(output, "Powered: yes") != NULL;
            if (frame.snapshot.bluetooth_enabled && run_command(bt_connected, NULL, output, sizeof output, 2500) == 0) {
                char *line, *save;
                for (line = strtok_r(output, "\n", &save); line && frame.snapshot.devices_count < MAX_ITEMS; line = strtok_r(NULL, "\n", &save))
                    if (!strncmp(line, "Device ", 7)) frame.snapshot.devices[frame.snapshot.devices_count++].connected = 1;
            }
        }
        if (run_command(volume, NULL, output, sizeof output, 2500) == 0 && (percent = strchr(output, '%'))) {
            char *begin = percent;
            while (begin > output && isdigit((unsigned char)begin[-1])) begin--;
            if (begin != percent) { frame.snapshot.audio_available = 1; frame.snapshot.volume = atoi(begin); }
        }
        if (frame.snapshot.audio_available && run_command(mute, NULL, output, sizeof output, 2500) == 0)
            frame.snapshot.muted = strstr(output, "yes") != NULL;
        if (!write_all(fd, &frame, sizeof frame)) break;
        ready = poll(&p, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (ready && (p.revents & POLLIN)) {
            size_t used = 0;
            while (used < sizeof req) {
                ssize_t n = read(fd, (char *)&req + used, sizeof req - used);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) goto done;
                used += (size_t)n;
            }
            if (execute(&req)) fprintf(stderr, "cui-bar: requested action failed\n");
            wipe(&req, sizeof req);
            frame.completed++;
        }
    }
done:
    close(fd); _exit(0);
}
static int start_worker(Bar *b) {
    int fd[2]; pid_t child;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) return 0;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC); fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    child = fork();
    if (child == 0) { close(fd[0]); close(ConnectionNumber(b->dpy)); worker(fd[1]); }
    close(fd[1]);
    if (child < 0) { close(fd[0]); return 0; }
    fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
    b->worker_fd = fd[0]; return 1;
}
static void audio_action(Bar *b, Action type, int value) {
    Request req = {0};
    if (b->worker_fd < 0 || b->worker_busy) return;
    req.type = type; req.value = value;
    if (send(b->worker_fd, &req, sizeof req, MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof req) {
        b->worker_busy = 1; b->worker_waiting = b->worker_completed + 1;
    } else fprintf(stderr, "cui-bar: could not queue action\n");
}
static void send_command(Bar *b, long command, long value) {
    XEvent e; memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage; e.xclient.window = b->root;
    e.xclient.message_type = b->command_atom; e.xclient.format = 32;
    e.xclient.data.l[0] = command; e.xclient.data.l[1] = value;
    XSendEvent(b->dpy, b->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(b->dpy);
}
static void read_workspace(Bar *b) {
    Atom type; int format; unsigned long n, left; unsigned char *data = NULL;
    if (XGetWindowProperty(b->dpy, b->root, b->state_atom, 0, 9, False, XA_CARDINAL,
                           &type, &format, &n, &left, &data) == Success && data &&
        type == XA_CARDINAL && format == 32 && n >= 9 && ((unsigned long *)data)[0] == 1) {
        unsigned long *v = (unsigned long *)data;
        b->active = v[1]; b->occupied = v[2]; b->urgent = v[3];
        b->mx = (int32_t)v[4]; b->my = (int32_t)v[5]; b->mw = (int)v[6];
        b->tag_count = v[8] > 9 ? 9 : (int)v[8];
        if (b->mw > 0 && b->mw <= 32767 && b->mw != b->width) b->width = b->mw;
        XMoveResizeWindow(b->dpy, b->win, b->mx, b->my, b->width, HEIGHT);
        position_icons(b);
    }
    if (data) XFree(data);
}
static int read_int(const char *path) {
    FILE *f = fopen(path, "r"); int n = -1;
    if (f) { if (fscanf(f, "%d", &n) != 1) n = -1; fclose(f); }
    return n;
}
static void read_stats(Bar *b) {
    FILE *f; char line[256]; unsigned long long v[8] = {0}, total = 0, idle; int i;
    unsigned long mem_total = 0, mem_available = 0;
    glob_t paths = {0}; time_t now = time(NULL); struct tm tm;
    localtime_r(&now, &tm); strftime(b->clock, sizeof b->clock, "%H:%M", &tm);
    f = fopen("/proc/stat", "r");
    if (f) {
        if (fgets(line, sizeof line, f) && sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
            for (i = 0; i < 8; i++) total += v[i];
            idle = v[3] + v[4];
            if (b->cpu_total && total > b->cpu_total && idle >= b->cpu_idle &&
                idle - b->cpu_idle <= total - b->cpu_total)
                b->cpu = (int)(100 * ((total - b->cpu_total) - (idle - b->cpu_idle)) / (total - b->cpu_total));
            b->cpu_total = total; b->cpu_idle = idle;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %lu", &mem_total);
            sscanf(line, "MemAvailable: %lu", &mem_available);
        }
        fclose(f);
        if (mem_total && mem_available <= mem_total) b->memory = (int)(100 * (mem_total - mem_available) / mem_total);
    }
    b->temperature = -1; b->battery = -1;
    if (!glob("/sys/class/thermal/thermal_zone*/temp", 0, NULL, &paths)) {
        for (i = 0; i < (int)paths.gl_pathc; i++) {
            int temp = read_int(paths.gl_pathv[i]);
            if (temp > 1000 && temp < 150000) { b->temperature = temp / 1000; break; }
        }
    }
    globfree(&paths); memset(&paths, 0, sizeof paths);
    if (!glob("/sys/class/power_supply/BAT*/capacity", 0, NULL, &paths) && paths.gl_pathc)
        b->battery = read_int(paths.gl_pathv[0]);
    globfree(&paths);
    {
        XkbStateRec state; Atom actual; int format; unsigned long n, left; unsigned char *data = NULL;
        snprintf(b->language, sizeof b->language, "--");
        if (XkbGetState(b->dpy, XkbUseCoreKbd, &state) == Success &&
            XGetWindowProperty(b->dpy, b->root, b->xkb_rules, 0, 4096, False, XA_STRING,
                               &actual, &format, &n, &left, &data) == Success &&
            actual == XA_STRING && format == 8 && data) {
            size_t pos = 0, end; unsigned int group = 0;
            for (i = 0; i < 2 && pos < n; i++) { while (pos < n && data[pos]) pos++; if (pos < n) pos++; }
            while (pos < n && data[pos] && group < state.group) {
                while (pos < n && data[pos] && data[pos] != ',') pos++;
                if (pos < n && data[pos] == ',') { pos++; group++; } else break;
            }
            end = pos; while (end < n && data[end] && data[end] != ',') end++;
            if (group == state.group && end > pos) {
                size_t len = end - pos; if (len >= sizeof b->language) len = sizeof b->language - 1;
                memcpy(b->language, data + pos, len); b->language[len] = 0;
                if (!strcmp(b->language, "ir")) strcpy(b->language, "fa");
                for (pos = 0; b->language[pos]; pos++) b->language[pos] = (char)toupper((unsigned char)b->language[pos]);
            }
        }
        if (data) XFree(data);
    }
}

/* Geometry for a centered workspace group and right-aligned modules. */
typedef struct { int stats, tags, audio, network, bluetooth, tray, clock, language, lock; } Layout;
static Layout layout(const Bar *b) {
    Layout l; int right = b->width - PAD;
    l.lock = right - 32; right = l.lock - 4;
    l.language = right - 38; right = l.language - 4;
    l.clock = right - 70; right = l.clock - 4;
    l.tray = right - b->tray_width; right = l.tray - (b->tray_width ? 4 : 0);
    l.bluetooth = right - 76; right = l.bluetooth - 4;
    l.network = right - 100; right = l.network - 4;
    l.audio = right - 80;
    l.tags = (b->width - b->tag_count * 28) / 2;
    l.stats = (l.tags >= 8 + 4 * 70 && l.tags + b->tag_count * 28 + 8 <= l.audio);
    if (l.tags + b->tag_count * 28 + 8 > l.audio) l.tags = 8;
    return l;
}
static void pill(cui_ctx *ui, int x, int w, const char *text, cui_color fg) {
    cui_rect rect = {(float)x, 5, (float)w, HEIGHT - 10};
    cui_draw_rect_border(ui, rect, CUI_CORNERS(13), CUI_RGB(0x12141a), CUI_RGB(0x1e2025), 1);
    cui_push_clip(ui, (cui_rect){rect.x + 5, rect.y, rect.w - 10, rect.h});
    cui_draw_text(ui, CUI_FONT_MONO, 12, (cui_vec2){rect.x + 9, 10}, fg, text, -1);
    cui_pop_clip(ui);
}
static void paint(cui_ctx *ui, cui_rect r, void *user) {
    Bar *b = *(Bar **)user; Layout l = layout(b); int i; char text[180];
    cui_color fg = CUI_RGB(0xadbac7), muted = CUI_RGB(0x64778d);
    (void)r;
    cui_draw_rect(ui, (cui_rect){0, 0, (float)b->width, HEIGHT}, CUI_CORNERS(0), CUI_RGB(0x090b0d));
    cui_draw_rect(ui, (cui_rect){0, HEIGHT - 2, (float)b->width, 2}, CUI_CORNERS(0), CUI_RGB(0x507396));
    if (l.stats) {
        if (b->temperature >= 0) snprintf(text, sizeof text, "%d C", b->temperature); else strcpy(text, "-- C");
        pill(ui, 8, 66, text, muted);
        if (b->cpu >= 0) snprintf(text, sizeof text, "CPU %d%%", b->cpu); else strcpy(text, "CPU --");
        pill(ui, 78, 66, text, fg);
        if (b->memory >= 0) snprintf(text, sizeof text, "RAM %d%%", b->memory); else strcpy(text, "RAM --");
        pill(ui, 148, 66, text, fg);
        if (b->battery >= 0) snprintf(text, sizeof text, "BAT %d%%", b->battery); else strcpy(text, "BAT --");
        pill(ui, 218, 66, text, b->battery >= 0 && b->battery <= 15 ? CUI_RGB(0xb86a6a) : fg);
    }
    if (l.tags >= 0 && l.tags + b->tag_count * 28 <= l.audio) {
        cui_draw_rect_border(ui, (cui_rect){l.tags - 4, 5, b->tag_count * 28 + 8, HEIGHT - 10}, CUI_CORNERS(13), CUI_RGB(0x12141a), CUI_RGB(0x1e2025), 1);
        for (i = 0; i < b->tag_count; i++) {
            int x = l.tags + i * 28; unsigned long mask = 1ul << i;
            if (b->active & mask) cui_draw_rect(ui, (cui_rect){x + 1, 7, 26, HEIGHT - 14}, CUI_CORNERS(11), CUI_RGB(0x8a949e));
            snprintf(text, sizeof text, "%d", i + 1);
            cui_draw_text(ui, CUI_FONT_MONO, 12, (cui_vec2){x + 10, 10},
                          b->urgent & mask ? CUI_RGB(0xb86a6a) : b->active & mask ? CUI_RGB(0x090b0d) : b->occupied & mask ? fg : muted, text, -1);
        }
    }
    if (b->snapshot.audio_available) snprintf(text, sizeof text, b->snapshot.muted ? "MUTED" : "VOL %d%%", b->snapshot.volume);
    else strcpy(text, "VOL --");
    pill(ui, l.audio, 80, text, b->snapshot.muted ? muted : fg);
    if (!b->network_available) strcpy(text, "NET --");
    else if (b->ethernet) strcpy(text, "ETHERNET");
    else if (b->snapshot.wifi_device[0]) {
        int strength = -1;
        for (i = 0; i < b->snapshot.networks_count; i++) if (b->snapshot.networks[i].active) { strength = b->snapshot.networks[i].signal; break; }
        if (strength >= 0) snprintf(text, sizeof text, "WIFI %d%%", strength); else strcpy(text, "WIFI ON");
    } else if (b->snapshot.wifi_available && !b->snapshot.wifi_enabled) strcpy(text, "WIFI OFF");
    else strcpy(text, "OFFLINE");
    pill(ui, l.network, 100, text, !b->network_available ? muted : !b->ethernet && !b->snapshot.wifi_device[0] ? CUI_RGB(0xb86a6a) : fg);
    if (!b->snapshot.bluetooth_available) strcpy(text, "BT --");
    else if (!b->snapshot.bluetooth_enabled) strcpy(text, "BT OFF");
    else {
        int connected = 0;
        for (i = 0; i < b->snapshot.devices_count; i++) connected += b->snapshot.devices[i].connected;
        if (connected) snprintf(text, sizeof text, "BT %d", connected); else strcpy(text, "BT ON");
    }
    pill(ui, l.bluetooth, 76, text, b->snapshot.bluetooth_enabled ? fg : muted);
    if (b->tray_width) cui_draw_rect_border(ui, (cui_rect){l.tray, 5, b->tray_width, HEIGHT - 10}, CUI_CORNERS(13), CUI_RGB(0x12141a), CUI_RGB(0x1e2025), 1);
    pill(ui, l.clock, 70, b->clock, fg); pill(ui, l.language, 38, b->language, fg);
    pill(ui, l.lock, 32, "L", muted);
}
static int draw(Bar *b) {
    XImage *image = XCreateImage(b->dpy, DefaultVisual(b->dpy, b->screen), DefaultDepth(b->dpy, b->screen),
                                 ZPixmap, 0, NULL, b->width, HEIGHT, 32, 0);
    if (!image) return 0;
    if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst || image->red_mask != 0xff0000 ||
        image->green_mask != 0xff00 || image->blue_mask != 0xff) { XDestroyImage(image); return 0; }
    image->data = calloc((size_t)image->bytes_per_line, HEIGHT);
    if (!image->data) { XDestroyImage(image); return 0; }
    cui_set_viewport(b->ui, (float)b->width, HEIGHT, 1.0f);
    cui_begin_frame(b->ui, 0.016f);
    cui_canvas(b->ui, "bar", CUI_FILL, CUI_FILL, paint, &b, sizeof b);
    cui_end_frame(b->ui);
    cui_sw_render(b->renderer, cui_get_draw_data(b->ui), image->data, b->width, HEIGHT, image->bytes_per_line, CUI_SW_BGRA);
    XPutImage(b->dpy, b->win, b->gc, image, 0, 0, 0, 0, b->width, HEIGHT);
    XDestroyImage(image); XFlush(b->dpy); return 1;
}
static void clicked(Bar *b, XButtonEvent *e) {
    Layout l = layout(b); int x = e->x;
    if (l.tags >= 0 && l.tags + b->tag_count * 28 <= l.audio && x >= l.tags && x < l.tags + b->tag_count * 28 && e->button == Button1)
        send_command(b, 1, 1l << ((x - l.tags) / 28));
    else if (x >= l.audio && x < l.audio + 80) {
        if (e->button == Button1) launch("pavucontrol");
        else if (e->button == Button3 && b->snapshot.audio_available) audio_action(b, ACT_AUDIO_MUTE, 0);
        else if ((e->button == Button4 || e->button == Button5) && b->snapshot.audio_available) {
            int value = b->snapshot.volume + (e->button == Button4 ? 2 : -2);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            audio_action(b, ACT_AUDIO_VOLUME, value);
        }
    } else if (x >= l.network && x < l.network + 100) {
        if (e->button == Button1) launch("nm-connection-editor");
        else if (e->button == Button3 && b->snapshot.wifi_available)
            audio_action(b, b->snapshot.wifi_enabled ? ACT_WIFI_OFF : ACT_WIFI_ON, 0);
    } else if (x >= l.bluetooth && x < l.bluetooth + 76) {
        if (e->button == Button1) launch("blueman-manager");
        else if (e->button == Button3 && b->snapshot.bluetooth_available)
            audio_action(b, b->snapshot.bluetooth_enabled ? ACT_BT_OFF : ACT_BT_ON, 0);
    }
    else if (x >= l.lock && e->button == Button1) launch("mel");
}

int main(void) {
    Bar b = {0}; XSetWindowAttributes attr; WorkerFrame incoming; size_t received = 0;
    time_t next_stats = 0; int dirty = 1, i, result = 0;
    cui_renderer_iface iface;
    b.worker_fd = -1; b.cpu = b.memory = b.temperature = b.battery = -1;
    b.tag_count = 9; strcpy(b.clock, "--:--"); strcpy(b.language, "--");
    signal(SIGINT, terminate); signal(SIGTERM, terminate); signal(SIGCHLD, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    b.dpy = XOpenDisplay(NULL);
    if (!b.dpy) { fprintf(stderr, "cui-bar: cannot open X display\n"); return 1; }
    fcntl(ConnectionNumber(b.dpy), F_SETFD, FD_CLOEXEC);
    XSetErrorHandler(xerror);
    b.screen = DefaultScreen(b.dpy); b.root = RootWindow(b.dpy, b.screen);
    b.width = DisplayWidth(b.dpy, b.screen);
    b.state_atom = XInternAtom(b.dpy, "_DWM_BAR_STATE", False);
    b.command_atom = XInternAtom(b.dpy, "_DWM_BAR_COMMAND", False);
    b.wm_atom = XInternAtom(b.dpy, "_NET_SUPPORTING_WM_CHECK", False);
    b.xkb_rules = XInternAtom(b.dpy, "_XKB_RULES_NAMES", False);
    attr.override_redirect = True; attr.background_pixel = BlackPixel(b.dpy, b.screen);
    attr.event_mask = ExposureMask | ButtonPressMask | SubstructureNotifyMask | SubstructureRedirectMask;
    b.win = XCreateWindow(b.dpy, b.root, 0, 0, b.width, HEIGHT, 0, DefaultDepth(b.dpy, b.screen),
                          InputOutput, DefaultVisual(b.dpy, b.screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
    b.gc = XCreateGC(b.dpy, b.win, 0, NULL);
    b.renderer = cui_sw_create();
    if (!b.renderer) { fprintf(stderr, "cui-bar: renderer initialization failed\n"); result = 1; goto cleanup; }
    cui_sw_set_threads(b.renderer, 1);
    iface = cui_sw_iface(b.renderer);
    b.ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = CUI_THEME_DARK });
    if (!b.ui) { fprintf(stderr, "cui-bar: CUI initialization failed\n"); result = 1; goto cleanup; }
    {
        const char *font = getenv("CUI_BAR_FONT");
        if (font && *font) {
            cui_font id = cui_font_load_file(b.ui, font);
            if (id >= 0) cui_font_set_role(b.ui, CUI_FONT_MONO, id);
        }
    }
    if (!acquire_tray(&b)) { result = 1; goto cleanup; }
    XSelectInput(b.dpy, b.root, PropertyChangeMask | StructureNotifyMask);
    read_workspace(&b);
    XMapRaised(b.dpy, b.win);
    send_command(&b, 2, (long)b.win);
    if (!start_worker(&b)) fprintf(stderr, "cui-bar: could not start service worker\n");
    while (running) {
        fd_set fds; int xfd = ConnectionNumber(b.dpy), maxfd = xfd, ready;
        struct timeval timeout; time_t now = time(NULL);
        if (now >= next_stats) { read_stats(&b); next_stats = now + 5; dirty = 1; }
        if (dirty) {
            if (!draw(&b)) { fprintf(stderr, "cui-bar: unsupported X11 visual or allocation failure\n"); result = 1; break; }
            dirty = 0;
        }
        FD_ZERO(&fds); FD_SET(xfd, &fds);
        if (b.worker_fd >= 0) { FD_SET(b.worker_fd, &fds); if (b.worker_fd > maxfd) maxfd = b.worker_fd; }
        timeout.tv_sec = next_stats > now ? next_stats - now : 0; timeout.tv_usec = 0;
        ready = XPending(b.dpy) ? 0 : select(maxfd + 1, &fds, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) { fprintf(stderr, "cui-bar: select: %s\n", strerror(errno)); result = 1; break; }
        if (ready > 0 && b.worker_fd >= 0 && FD_ISSET(b.worker_fd, &fds)) {
            ssize_t n = read(b.worker_fd, (char *)&incoming + received, sizeof incoming - received);
            if (n > 0) {
                received += (size_t)n;
                if (received == sizeof incoming) {
                    b.snapshot = incoming.snapshot; b.ethernet = incoming.ethernet; b.network_available = incoming.network_available;
                    b.worker_completed = incoming.completed;
                    if (b.worker_completed >= b.worker_waiting) b.worker_busy = 0;
                    received = 0; dirty = 1;
                }
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "cui-bar: service worker stopped\n"); close(b.worker_fd); b.worker_fd = -1;
            }
        }
        while (XPending(b.dpy)) {
            XEvent e; int index;
            XNextEvent(b.dpy, &e);
            if (e.type == Expose && e.xexpose.window == b.win && e.xexpose.count == 0) dirty = 1;
            else if (e.type == ButtonPress && e.xbutton.window == b.win) clicked(&b, &e.xbutton);
            else if (e.type == ClientMessage && e.xclient.window == b.win && e.xclient.message_type == b.opcode && e.xclient.data.l[1] == 0) {
                dock(&b, (Window)e.xclient.data.l[2]); dirty = 1;
            } else if (e.type == SelectionClear && e.xselectionclear.selection == b.selection) {
                fprintf(stderr, "cui-bar: system tray ownership lost\n"); running = 0;
            } else if (e.type == DestroyNotify && (index = icon_index(&b, e.xdestroywindow.window)) >= 0) {
                remove_icon(&b, index); dirty = 1;
            } else if (e.type == PropertyNotify && e.xproperty.window == b.root && e.xproperty.atom == b.state_atom) {
                read_workspace(&b); dirty = 1;
            } else if (e.type == PropertyNotify && e.xproperty.window == b.root && e.xproperty.atom == b.wm_atom) {
                send_command(&b, 2, (long)b.win);
            } else if (e.type == PropertyNotify && (index = icon_index(&b, e.xproperty.window)) >= 0 && e.xproperty.atom == b.xembed_info) {
                int mapped = icon_mapped(&b, b.icons[index].win);
                if (mapped != b.icons[index].mapped) { b.icons[index].mapped = mapped; position_icons(&b); dirty = 1; }
            } else if (e.type == MapRequest && (index = icon_index(&b, e.xmaprequest.window)) >= 0) {
                if (b.icons[index].mapped) XMapWindow(b.dpy, b.icons[index].win);
            } else if (e.type == ResizeRequest && icon_index(&b, e.xresizerequest.window) >= 0) position_icons(&b);
            else if (e.type == ConfigureRequest && icon_index(&b, e.xconfigurerequest.window) >= 0) position_icons(&b);
            else if (e.type == ConfigureNotify && e.xconfigure.window == b.root) { read_workspace(&b); dirty = 1; }
        }
    }
cleanup:
    if (b.worker_fd >= 0) close(b.worker_fd);
    if (b.selection && XGetSelectionOwner(b.dpy, b.selection) == b.win) XSetSelectionOwner(b.dpy, b.selection, None, CurrentTime);
    for (i = 0; i < b.count; i++) {
        XReparentWindow(b.dpy, b.icons[i].win, b.root, 0, 0);
        XRemoveFromSaveSet(b.dpy, b.icons[i].win);
    }
    if (b.ui) cui_destroy(b.ui);
    if (b.renderer) cui_sw_destroy(b.renderer);
    if (b.gc) XFreeGC(b.dpy, b.gc);
    if (b.win) XDestroyWindow(b.dpy, b.win);
    XCloseDisplay(b.dpy);
    return result;
}
