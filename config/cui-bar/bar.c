/* Standalone CUI/Xlib bar, popup and XEmbed tray; small X11 IPC with dwm. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
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
#include "../dwm/dashboard_backend.h"

#define HEIGHT 36
#define ICON_SIZE 24
#define ICON_SLOT 30
#define MAX_ICONS 64
#define PAD 8
#define BUTTON_WIDTH 92
#define RIGHT_MODULES 336
#define TRAY_PAD 8
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_MAPPED 1

typedef struct { Window win; int mapped; } TrayIcon;
typedef struct { Window win; int id, x, y, width, height; unsigned long active, occupied, urgent; } Panel;
typedef struct {
    Display *dpy;
    int screen, width;
    Window root, win;
    GC gc;
    Atom selection, manager, opcode, orientation, visual, xembed, xembed_info;
    Atom state_atom, command_atom, wm_atom, xkb_rules;
    unsigned long active, occupied, urgent;
    int mx, my, mw, tag_count, worker_fd, worker_busy, worker_completed, worker_waiting;
    Panel panels[16]; int panel_count, current;
    Snapshot snapshot;
    int ethernet, network_available, cpu, memory, temperature, battery;
    unsigned long long cpu_total, cpu_idle;
    char clock[32], language[12];
    TrayIcon icons[MAX_ICONS];
    int count, tray_width;
    cui_sw_renderer *renderer;
    cui_ctx *ui;
    Window popup; int popup_width, popup_height, popup_page, popup_dirty;
    cui_ctx *popup_ui;
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
    int limit = (b->width - (24 + b->tag_count * 28 + RIGHT_MODULES)) / ICON_SLOT;
    if (limit < 0) limit = 0;
    if (limit > 8) limit = 8;
    for (i = 0; i < b->count; i++) if (b->icons[i].mapped && n < limit) n++;
    b->tray_width = n ? n * ICON_SLOT + TRAY_PAD : 0;
    x = b->width - RIGHT_MODULES - b->tray_width + TRAY_PAD / 2;
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
    char message[256] = "";
    struct pollfd p = { .fd = fd, .events = POLLIN };
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_IGN);
    setenv("LC_ALL", "C", 1);
    for (;;) {
        int ready;
        read_snapshot(&frame.snapshot);
        copystr(frame.snapshot.message, sizeof frame.snapshot.message, message);
        frame.ethernet = frame.network_available = 0;
        if (run_command(devices, NULL, output, sizeof output, 2500) == 0) {
            frame.network_available = 1;
            frame.ethernet = strstr(output, "ethernet:connected") != NULL;
        }
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
            if (execute(&req)) {
                if (req.type == ACT_WIFI_CONNECT) copystr(message, sizeof message, "Connection failed. Check password or NetworkManager.");
                else if (req.type == ACT_BT_PAIR) copystr(message, sizeof message, "Pairing failed. A Bluetooth pairing agent may be required.");
                else copystr(message, sizeof message, "Action failed. Check device and permissions.");
            } else copystr(message, sizeof message, "Done.");
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
static void send_command(Bar *b, long command, long value, int monitor) {
    XEvent e; memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage; e.xclient.window = b->root;
    e.xclient.message_type = b->command_atom; e.xclient.format = 32;
    e.xclient.data.l[0] = command; e.xclient.data.l[1] = value; e.xclient.data.l[2] = monitor;
    XSendEvent(b->dpy, b->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(b->dpy);
}
static void use_panel(Bar *b, int i) {
    Panel *p = &b->panels[i];
    b->current = i; b->win = p->win; b->width = p->width;
    b->mx = p->x; b->my = p->y; b->mw = p->width;
    b->active = p->active; b->occupied = p->occupied; b->urgent = p->urgent;
}
static int panel_index(Bar *b, Window w) {
    int i; for (i = 0; i < b->panel_count; i++) if (b->panels[i].win == w) return i;
    return -1;
}
static void read_workspace(Bar *b) {
    Atom type; int format; unsigned long n, left; unsigned char *data = NULL;
    if (XGetWindowProperty(b->dpy, b->root, b->state_atom, 0, 3 + 16 * 8, False, XA_CARDINAL,
                           &type, &format, &n, &left, &data) == Success && data &&
        type == XA_CARDINAL && format == 32 && n >= 3 && ((unsigned long *)data)[0] == 2) {
        unsigned long *v = (unsigned long *)data;
        int count = (int)v[1], i;
        if (count > 16) count = 16;
        if (count < 1 || n < (unsigned long)(3 + count * 8)) goto done;
        b->tag_count = v[2] > 9 ? 9 : (int)v[2];
        for (i = 0; i < count; i++) {
            int at = 3 + i * 8, old_id = b->panels[i].id;
            Panel *p = &b->panels[i];
            int is_new = i >= b->panel_count;
            int was_uninitialized = p->width <= 1;
            if (is_new) {
                XSetWindowAttributes attr;
                attr.override_redirect = True; attr.background_pixel = BlackPixel(b->dpy, b->screen);
                attr.event_mask = ExposureMask | ButtonPressMask;
                p->win = XCreateWindow(b->dpy, b->root, 0, 0, 1, HEIGHT, 0, DefaultDepth(b->dpy, b->screen),
                                       InputOutput, DefaultVisual(b->dpy, b->screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
            }
            p->id = (int)v[at]; p->active = v[at + 1]; p->occupied = v[at + 2]; p->urgent = v[at + 3];
            p->x = (int32_t)v[at + 4]; p->y = (int32_t)v[at + 5];
            p->width = (int)v[at + 6]; p->height = (int)v[at + 7];
            if (p->width <= 0 || p->width > 32767) p->width = 1;
            XMoveResizeWindow(b->dpy, p->win, p->x, p->y, p->width, HEIGHT);
            XMapRaised(b->dpy, p->win);
            if (is_new || old_id != p->id || was_uninitialized) send_command(b, 2, (long)p->win, p->id);
        }
        for (i = count; i < b->panel_count; i++) XDestroyWindow(b->dpy, b->panels[i].win);
        b->panel_count = count;
        use_panel(b, 0); position_icons(b);
    }
done:
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

/* The control center belongs to the bar process: one popup, one event loop,
 * and the same asynchronous worker used by the status modules. */
static Bar *popup_bar;
static bool request_action(Action action, const char *arg, const char *password, int value) {
    Request req = {0}; bool ok = false; Bar *b = popup_bar;
    if (!b || b->worker_fd < 0 || b->worker_busy) return false;
    req.type = action; req.value = value;
    copystr(req.arg, sizeof req.arg, arg); copystr(req.password, sizeof req.password, password);
    if (send(b->worker_fd, &req, sizeof req, MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof req) {
        b->worker_busy = 1; b->worker_waiting = b->worker_completed + 1; ok = true;
    }
    if (!ok) copystr(b->snapshot.message, sizeof b->snapshot.message, "Could not send action to the service worker.");
    wipe(&req, sizeof req); return ok;
}
static void open_settings(const char *program) { launch(program); }
#include "popup_content.h"
static UIState popup_state;
static void close_popup(Bar *b) {
    if (!b->popup) return;
    XUngrabKeyboard(b->dpy, CurrentTime);
    XDestroyWindow(b->dpy, b->popup); b->popup = None;
    if (b->popup_ui) { cui_destroy(b->popup_ui); b->popup_ui = NULL; }
    wipe(&popup_state, sizeof popup_state); popup_bar = NULL;
}
static void open_popup(Bar *b, int page) {
    XSetWindowAttributes attr; cui_renderer_iface iface;
    Panel *p = &b->panels[b->current];
    int width = p->width < 490 ? p->width : 490;
    int height = p->height - HEIGHT - 12;
    if (height > 650) height = 650;
    if (height < 100 || width < 100) return;
    if (b->popup) { close_popup(b); }
    attr.override_redirect = True; attr.background_pixel = BlackPixel(b->dpy, b->screen);
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                      LeaveWindowMask | KeyPressMask | KeyReleaseMask;
    b->popup = XCreateWindow(b->dpy, b->root, p->x + p->width - width, p->y + HEIGHT,
                            width, height, 0, DefaultDepth(b->dpy, b->screen), InputOutput,
                            DefaultVisual(b->dpy, b->screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
    if (!b->popup) return;
    iface = cui_sw_iface(b->renderer);
    b->popup_ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = CUI_THEME_DARK });
    if (!b->popup_ui) { XDestroyWindow(b->dpy, b->popup); b->popup = None; return; }
    style(b->popup_ui);
    cui_font_set_role(b->popup_ui, CUI_FONT_BODY, CUI_FONT_MONO);
    cui_font_set_role(b->popup_ui, CUI_FONT_MEDIUM, CUI_FONT_MONO);
    b->popup_width = width; b->popup_height = height;
    memset(&popup_state, 0, sizeof popup_state); popup_state.page = page;
    popup_bar = b; b->popup_dirty = 1;
    XMapRaised(b->dpy, b->popup); XSync(b->dpy, False);
    if (XGrabKeyboard(b->dpy, b->popup, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
        fprintf(stderr, "cui-bar: cannot grab keyboard for control center\n"); close_popup(b); return;
    }
    cui_input_focus(b->popup_ui, true);
}
static int draw_popup(Bar *b) {
    static const char *pages[] = {"Wi-Fi", "Bluetooth", "Sound"};
    XImage *image;
    cui_ctx *ui = b->popup_ui;
    if (!b->popup || !ui) return 1;
    image = XCreateImage(b->dpy, DefaultVisual(b->dpy, b->screen), DefaultDepth(b->dpy, b->screen),
                         ZPixmap, 0, NULL, b->popup_width, b->popup_height, 32, 0);
    if (!image) return 0;
    if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst || image->red_mask != 0xff0000 ||
        image->green_mask != 0xff00 || image->blue_mask != 0xff) { XDestroyImage(image); return 0; }
    image->data = calloc((size_t)image->bytes_per_line, b->popup_height);
    if (!image->data) { XDestroyImage(image); return 0; }
    cui_set_viewport(ui, b->popup_width, b->popup_height, 1);
    cui_begin_frame(ui, 0.05f);
    cui_scroll(ui, .id = "controls", .width = CUI_FILL, .height = CUI_FILL, .pad = 18, .gap = 12) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .width = CUI_FILL, .gap = 3) {
                cui_text(ui, "QUICK CONTROLS", .font = CUI_FONT_MEDIUM, .size = 16);
                cui_text(ui, "NETWORK  /  DEVICES  /  AUDIO", .size = 10, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            if (cui_button(ui, "Close", .variant = CUI_BUTTON_GHOST)) b->popup_page = -1;
        }
        cui_segmented(ui, "pages", pages, 3, &popup_state.page, .full_width = true);
        if (b->worker_busy) cui_text(ui, "Working...", .color = CUI_COLOR_INFO);
        else if (b->snapshot.message[0]) cui_text(ui, b->snapshot.message, .color = CUI_COLOR_TEXT_SECONDARY);
        if (popup_state.page == 0) wifi_page(ui, &popup_state, &b->snapshot, b->worker_busy);
        else if (popup_state.page == 1) bluetooth_page(ui, &popup_state, &b->snapshot, b->worker_busy);
        else audio_page(ui, &popup_state, &b->snapshot, b->worker_busy);
    }
    cui_end_frame(ui);
    cui_sw_render(b->renderer, cui_get_draw_data(ui), image->data, b->popup_width, b->popup_height, image->bytes_per_line, CUI_SW_BGRA);
    XPutImage(b->dpy, b->popup, b->gc, image, 0, 0, 0, 0, b->popup_width, b->popup_height);
    XDestroyImage(image); XFlush(b->dpy);
    if (b->popup_page == -1) { b->popup_page = 0; close_popup(b); }
    return 1;
}
static cui_key popup_key(KeySym key) {
    if (key >= XK_a && key <= XK_z) return (cui_key)(CUI_KEY_A + key - XK_a);
    if (key >= XK_A && key <= XK_Z) return (cui_key)(CUI_KEY_A + key - XK_A);
    if (key >= XK_0 && key <= XK_9) return (cui_key)(CUI_KEY_0 + key - XK_0);
    switch (key) {
    case XK_Tab: return CUI_KEY_TAB; case XK_Return: case XK_KP_Enter: return CUI_KEY_ENTER;
    case XK_BackSpace: return CUI_KEY_BACKSPACE; case XK_Delete: return CUI_KEY_DELETE;
    case XK_space: return CUI_KEY_SPACE; case XK_Left: return CUI_KEY_LEFT; case XK_Right: return CUI_KEY_RIGHT;
    case XK_Up: return CUI_KEY_UP; case XK_Down: return CUI_KEY_DOWN;
    case XK_Home: return CUI_KEY_HOME; case XK_End: return CUI_KEY_END;
    case XK_Page_Up: return CUI_KEY_PAGE_UP; case XK_Page_Down: return CUI_KEY_PAGE_DOWN;
    default: return CUI_KEY_NONE;
    }
}
static void popup_event(Bar *b, XEvent *e) {
    cui_ctx *ui = b->popup_ui;
    if (!ui) return;
    if (e->type == MotionNotify) cui_input_mouse_move(ui, e->xmotion.x, e->xmotion.y);
    else if (e->type == LeaveNotify) cui_input_mouse_leave(ui);
    else if (e->type == ButtonPress || e->type == ButtonRelease) {
        int down = e->type == ButtonPress; unsigned int button = e->xbutton.button;
        cui_input_mouse_move(ui, e->xbutton.x, e->xbutton.y);
        if (button == Button1) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, down);
        else if (button == Button2) cui_input_mouse_button(ui, CUI_MOUSE_MIDDLE, down);
        else if (button == Button3) cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, down);
        else if (down && button == Button4) cui_input_scroll(ui, 0, 1);
        else if (down && button == Button5) cui_input_scroll(ui, 0, -1);
    } else if (e->type == KeyPress || e->type == KeyRelease) {
        int down = e->type == KeyPress, mods = 0, n; char text[128]; KeySym sym;
        n = XLookupString(&e->xkey, text, sizeof text - 1, &sym, NULL);
        if (down && sym == XK_Escape) { close_popup(b); return; }
        if (e->xkey.state & ShiftMask) mods |= CUI_MOD_SHIFT;
        if (e->xkey.state & ControlMask) mods |= CUI_MOD_CTRL;
        if (e->xkey.state & Mod1Mask) mods |= CUI_MOD_ALT;
        if (e->xkey.state & Mod4Mask) mods |= CUI_MOD_SUPER;
        cui_input_key(ui, popup_key(sym), down, mods);
        if (down && n > 0 && !(mods & (CUI_MOD_CTRL | CUI_MOD_ALT | CUI_MOD_SUPER))) {
            text[n] = 0; cui_input_text(ui, text);
        }
        wipe(text, sizeof text);
    }
    if (b->popup && !draw_popup(b)) close_popup(b);
}

/* A left-hand workspace rail, optional telemetry, then a compact control dock.
 * The same geometry drives painting, hit testing and tray placement. */
typedef struct { int stats, tags, audio, network, bluetooth, tray, clock, language, lock; } Layout;
static Layout layout(const Bar *b) {
    Layout l; int right = b->width - PAD;
    l.lock = right - 28; right = l.lock - 4;
    l.clock = right - 64; right = l.clock - 4;
    l.language = right - 34; right = l.language - 4;
    l.bluetooth = right - 54; right = l.bluetooth - 4;
    l.network = right - 62; right = l.network - 4;
    l.audio = right - 62; right = l.audio - 4;
    l.tray = right - (b->current == 0 ? b->tray_width : 0);
    l.tags = 38;
    l.stats = l.tray - (l.tags + b->tag_count * 28) >= 300;
    return l;
}
static void label(cui_ctx *ui, int x, int y, int size, cui_color color, const char *text) {
    cui_draw_text(ui, CUI_FONT_MONO, size, (cui_vec2){x, y}, color, text, -1);
}
static void dock_cell(cui_ctx *ui, int x, int w, const char *title, const char *value, cui_color accent) {
    cui_draw_rect(ui, (cui_rect){x, 4, w, HEIGHT - 8}, CUI_CORNERS(5), CUI_RGB(0x12141a));
    cui_draw_rect(ui, (cui_rect){x + 4, 9, 2, 18}, CUI_CORNERS(1), accent);
    cui_push_clip(ui, (cui_rect){x + 9, 4, w - 10, HEIGHT - 8});
    label(ui, x + 10, 6, 9, CUI_RGB(0x64778d), title);
    label(ui, x + 10, 17, 11, CUI_RGB(0xadbac7), value);
    cui_pop_clip(ui);
}
static void paint(cui_ctx *ui, cui_rect r, void *user) {
    Bar *b = *(Bar **)user; Layout l = layout(b); int i; char text[48];
    cui_color fg = CUI_RGB(0xadbac7), muted = CUI_RGB(0x64778d), blue = CUI_RGB(0x7fa8c9), gold = CUI_RGB(0xf2d49e);
    (void)r;
    cui_draw_rect(ui, (cui_rect){0, 0, (float)b->width, HEIGHT}, CUI_CORNERS(0), CUI_RGB(0x090b0d));
    cui_draw_rect(ui, (cui_rect){0, HEIGHT - 1, (float)b->width, 1}, CUI_CORNERS(0), CUI_RGB(0x2d3035));
    cui_draw_rect(ui, (cui_rect){8, 8, 22, 20}, CUI_CORNERS(5), CUI_RGB(0x1c2834));
    label(ui, 13, 11, 11, blue, "W");
    if (l.tags + b->tag_count * 28 <= l.tray - 4) {
        for (i = 0; i < b->tag_count; i++) {
            int x = l.tags + i * 28; unsigned long mask = 1ul << i;
            if (b->active & mask) cui_draw_rect(ui, (cui_rect){x + 2, 5, 24, 26}, CUI_CORNERS(5), CUI_RGB(0x24313d));
            if (b->active & mask) cui_draw_rect(ui, (cui_rect){x + 8, 28, 12, 2}, CUI_CORNERS(1), gold);
            else if (b->occupied & mask) cui_draw_circle(ui, (cui_vec2){x + 14, 27}, 1.5f, blue);
            snprintf(text, sizeof text, "%d", i + 1);
            label(ui, x + 10, 10, 12, b->urgent & mask ? CUI_RGB(0xb86a6a) : b->active & mask ? gold : b->occupied & mask ? fg : muted, text);
        }
    }
    if (l.stats) {
        int x = l.tags + b->tag_count * 28 + 22;
        label(ui, x, 6, 9, muted, "SYSTEM");
        snprintf(text, sizeof text, "CPU %s", b->cpu < 0 ? "--" : "");
        if (b->cpu >= 0) snprintf(text, sizeof text, "CPU %d%%", b->cpu);
        label(ui, x, 17, 11, fg, text);
        if (b->memory >= 0) snprintf(text, sizeof text, "MEM %d%%", b->memory); else strcpy(text, "MEM --");
        label(ui, x + 75, 17, 11, fg, text);
        if (b->temperature >= 0) snprintf(text, sizeof text, "%d C", b->temperature); else strcpy(text, "-- C");
        label(ui, x + 150, 17, 11, muted, text);
        if (b->battery >= 0) { snprintf(text, sizeof text, "BAT %d%%", b->battery); label(ui, x + 205, 17, 11, b->battery <= 15 ? CUI_RGB(0xb86a6a) : fg, text); }
    }
    if (b->snapshot.audio_available) { if (b->snapshot.muted) strcpy(text, "MUTE"); else snprintf(text, sizeof text, "%d%%", b->snapshot.volume); } else strcpy(text, "--");
    dock_cell(ui, l.audio, 62, "SOUND", text, blue);
    if (!b->network_available) strcpy(text, "--"); else if (b->ethernet) strcpy(text, "LAN");
    else if (b->snapshot.wifi_device[0]) strcpy(text, "WIFI"); else strcpy(text, "OFF");
    dock_cell(ui, l.network, 62, "NET", text, b->ethernet || b->snapshot.wifi_device[0] ? blue : muted);
    if (!b->snapshot.bluetooth_available) strcpy(text, "--"); else if (!b->snapshot.bluetooth_enabled) strcpy(text, "OFF");
    else { int connected = 0; for (i = 0; i < b->snapshot.devices_count; i++) connected += b->snapshot.devices[i].connected;
        if (connected) snprintf(text, sizeof text, "%d ON", connected); else strcpy(text, "ON"); }
    dock_cell(ui, l.bluetooth, 54, "BT", text, b->snapshot.bluetooth_enabled ? blue : muted);
    label(ui, l.language + 5, 12, 11, muted, b->language);
    cui_draw_rect(ui, (cui_rect){l.clock, 4, 64, HEIGHT - 8}, CUI_CORNERS(5), CUI_RGB(0x1c2834));
    label(ui, l.clock + 11, 10, 13, gold, b->clock);
    label(ui, l.lock + 8, 11, 12, muted, "L");
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
    if (l.tags >= 0 && l.tags + b->tag_count * 28 <= l.tray - 4 && x >= l.tags && x < l.tags + b->tag_count * 28 && e->button == Button1)
        send_command(b, 1, 1l << ((x - l.tags) / 28), b->panels[b->current].id);
    else if (x >= l.audio && x < l.audio + 62) {
        if (e->button == Button1) open_popup(b, 2);
        else if (e->button == Button3 && b->snapshot.audio_available) audio_action(b, ACT_AUDIO_MUTE, 0);
        else if ((e->button == Button4 || e->button == Button5) && b->snapshot.audio_available) {
            int value = b->snapshot.volume + (e->button == Button4 ? 2 : -2);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            audio_action(b, ACT_AUDIO_VOLUME, value);
        }
    } else if (x >= l.network && x < l.network + 62) {
        if (e->button == Button1) open_popup(b, 0);
        else if (e->button == Button3 && b->snapshot.wifi_available)
            audio_action(b, b->snapshot.wifi_enabled ? ACT_WIFI_OFF : ACT_WIFI_ON, 0);
    } else if (x >= l.bluetooth && x < l.bluetooth + 54) {
        if (e->button == Button1) open_popup(b, 1);
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
    /* Do not show a root-sized fallback: the root can span several monitors. */
    b.width = 1;
    b.state_atom = XInternAtom(b.dpy, "_DWM_BAR_STATE", False);
    b.command_atom = XInternAtom(b.dpy, "_DWM_BAR_COMMAND", False);
    b.wm_atom = XInternAtom(b.dpy, "_NET_SUPPORTING_WM_CHECK", False);
    b.xkb_rules = XInternAtom(b.dpy, "_XKB_RULES_NAMES", False);
    attr.override_redirect = True; attr.background_pixel = BlackPixel(b.dpy, b.screen);
    attr.event_mask = ExposureMask | ButtonPressMask | SubstructureNotifyMask | SubstructureRedirectMask;
    b.win = XCreateWindow(b.dpy, b.root, 0, 0, b.width, HEIGHT, 0, DefaultDepth(b.dpy, b.screen),
                          InputOutput, DefaultVisual(b.dpy, b.screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
    b.panels[0].win = b.win; b.panels[0].id = 0; b.panels[0].width = b.width;
    b.panels[0].height = DisplayHeight(b.dpy, b.screen); b.panel_count = 1;
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
    if (b.panels[0].width > 1) {
        XMapRaised(b.dpy, b.win);
        send_command(&b, 2, (long)b.win, b.panels[0].id);
    } else fprintf(stderr, "cui-bar: waiting for dwm's version 2 monitor IPC; rebuild and restart dwm if it does not appear\n");
    if (!start_worker(&b)) fprintf(stderr, "cui-bar: could not start service worker\n");
    while (running) {
        fd_set fds; int xfd = ConnectionNumber(b.dpy), maxfd = xfd, ready;
        struct timeval timeout; time_t now = time(NULL);
        if (now >= next_stats) { read_stats(&b); next_stats = now + 5; dirty = 1; }
        if (dirty) {
            for (i = 0; i < b.panel_count; i++) {
                use_panel(&b, i);
                if (!draw(&b)) { fprintf(stderr, "cui-bar: unsupported X11 visual or allocation failure\n"); result = 1; running = 0; break; }
            }
            use_panel(&b, 0);
            if (!running) break;
            dirty = 0;
            if (b.popup) b.popup_dirty = 1;
        }
        if (b.popup && b.popup_dirty) { if (!draw_popup(&b)) close_popup(&b); b.popup_dirty = 0; }
        FD_ZERO(&fds); FD_SET(xfd, &fds);
        if (b.worker_fd >= 0) { FD_SET(b.worker_fd, &fds); if (b.worker_fd > maxfd) maxfd = b.worker_fd; }
        timeout.tv_sec = next_stats > now ? next_stats - now : 0; timeout.tv_usec = 0;
        if (b.popup && timeout.tv_sec > 0) { timeout.tv_sec = 0; timeout.tv_usec = 250000; }
        ready = XPending(b.dpy) ? 0 : select(maxfd + 1, &fds, NULL, NULL, &timeout);
        if (ready == 0 && b.popup) b.popup_dirty = 1;
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
            if (b.popup && e.xany.window == b.popup) { popup_event(&b, &e); continue; }
            if (e.type == Expose && panel_index(&b, e.xexpose.window) >= 0 && e.xexpose.count == 0) dirty = 1;
            else if (e.type == ButtonPress && (index = panel_index(&b, e.xbutton.window)) >= 0) {
                use_panel(&b, index); clicked(&b, &e.xbutton); use_panel(&b, 0);
            }
            else if (e.type == ClientMessage && e.xclient.window == b.win && e.xclient.message_type == b.opcode && e.xclient.data.l[1] == 0) {
                dock(&b, (Window)e.xclient.data.l[2]); dirty = 1;
            } else if (e.type == SelectionClear && e.xselectionclear.selection == b.selection) {
                fprintf(stderr, "cui-bar: system tray ownership lost\n"); running = 0;
            } else if (e.type == DestroyNotify && (index = icon_index(&b, e.xdestroywindow.window)) >= 0) {
                remove_icon(&b, index); dirty = 1;
            } else if (e.type == PropertyNotify && e.xproperty.window == b.root && e.xproperty.atom == b.state_atom) {
                read_workspace(&b); dirty = 1;
            } else if (e.type == PropertyNotify && e.xproperty.window == b.root && e.xproperty.atom == b.wm_atom) {
                for (i = 0; i < b.panel_count; i++) send_command(&b, 2, (long)b.panels[i].win, b.panels[i].id);
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
    close_popup(&b);
    if (b.worker_fd >= 0) close(b.worker_fd);
    if (b.selection && XGetSelectionOwner(b.dpy, b.selection) == b.win) XSetSelectionOwner(b.dpy, b.selection, None, CurrentTime);
    for (i = 0; i < b.count; i++) {
        XReparentWindow(b.dpy, b.icons[i].win, b.root, 0, 0);
        XRemoveFromSaveSet(b.dpy, b.icons[i].win);
    }
    if (b.ui) cui_destroy(b.ui);
    if (b.renderer) cui_sw_destroy(b.renderer);
    if (b.gc) XFreeGC(b.dpy, b.gc);
    for (i = 0; i < b.panel_count; i++) if (b.panels[i].win) XDestroyWindow(b.dpy, b.panels[i].win);
    XCloseDisplay(b.dpy);
    return result;
}
