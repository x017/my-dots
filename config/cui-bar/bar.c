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
#include <dirent.h>
#include <dlfcn.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "../dwm/dashboard_backend.h"
#include "../dwm/dashboard/desktop_controls.h"
#include "island.h"

#define HEIGHT 28
#define TOP_GAP 8
#define SIDE_GAP 10
#define RADIUS 8
#define ICON_SIZE 18
#define ICON_SLOT 30
#define MAX_ICONS 64
#define MAX_CPU_THREADS 128
#define PAD 8
#define BUTTON_WIDTH 92
#define NET_WIDTH(width) ((width) >= 850 ? 166 : 106)
#define RIGHT_MODULES(width) (372 + NET_WIDTH(width) - 62)
#define TRAY_PAD 8
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_MAPPED 1

typedef struct { Window win; int mapped; } TrayIcon;
typedef struct { Window win; int id, x, y, width, height, mapped, fullscreen; unsigned long active, occupied, urgent; } Panel;
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
    DesktopState desktop;
    int ethernet, network_available, cpu, memory, temperature, battery;
    unsigned long long cpu_total, cpu_idle, core_total[MAX_CPU_THREADS], core_idle[MAX_CPU_THREADS];
    int core_load[MAX_CPU_THREADS], core_id[MAX_CPU_THREADS], core_count, core_seen;
    unsigned long memory_total_kb, memory_used_kb;
    char clock[32], language[12];
    TrayIcon icons[MAX_ICONS];
    int count, tray_width;
    cui_sw_renderer *renderer;
    cui_ctx *ui;
    Window popup; int popup_width, popup_height, popup_page, popup_dirty, calendar_offset, popup_calendar, popup_notifications, popup_monitor;
    int island_fd, island_initialized, island_live; double island_notice_until; IslandFrame island;
    cui_ctx *popup_ui;
    struct { char path[512]; cui_texture texture; } notice_images[ISLAND_NOTICES];
    Window tooltip; cui_ctx *tooltip_ui; int tooltip_kind, tooltip_panel, tooltip_width, tooltip_height;
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
    int limit = (b->width - (24 + b->tag_count * 28 + RIGHT_MODULES(b->width) + 120)) / ICON_SLOT;
    if (limit < 0) limit = 0;
    if (limit > 8) limit = 8;
    for (i = 0; i < b->count; i++) if (b->icons[i].mapped && n < limit) n++;
    b->tray_width = n ? n * ICON_SLOT + TRAY_PAD : 0;
    x = b->width - RIGHT_MODULES(b->width) - b->tray_width + TRAY_PAD / 2;
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

typedef struct { Snapshot snapshot; DesktopState desktop; int ethernet, network_available, completed; } WorkerFrame;
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
    char message[256] = ""; time_t next_desktop = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_IGN);
    setenv("LC_ALL", "C", 1);
    for (;;) {
        int ready;
        read_snapshot(&frame.snapshot);
        if (time(NULL) >= next_desktop) { desktop_read(&frame.desktop); next_desktop = time(NULL) + 10; }
        copystr(frame.snapshot.message, sizeof frame.snapshot.message, message);
        frame.ethernet = frame.network_available = 0;
        if (run_command(devices, NULL, output, sizeof output, 2500) == 0) {
            frame.network_available = 1;
            frame.ethernet = strstr(output, "ethernet:connected") != NULL;
        }
        if (!write_all(fd, &frame, sizeof frame)) break;
        ready = poll(&p, 1, 2000);
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
            int rc; message[0] = 0; dashboard_action_error[0] = 0;
            if (req.type == ACT_DISPLAY_APPLY) {
                message[0] = 0; rc = desktop_execute(&req, message, sizeof message); next_desktop = 0;
            } else rc = execute(&req);
            if (rc && !message[0]) {
                if (dashboard_action_error[0]) copystr(message, sizeof message, dashboard_action_error);
                else if (req.type == ACT_WIFI_CONNECT) copystr(message, sizeof message, "Connection failed. Check password or NetworkManager.");
                else if (req.type == ACT_BT_PAIR) copystr(message, sizeof message, "Pairing failed. A Bluetooth pairing agent may be required.");
                else copystr(message, sizeof message, "Action failed. Check device and permissions.");
            } else if (!rc && req.type != ACT_DISPLAY_APPLY) copystr(message, sizeof message, "Done.");
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
static void read_stats(Bar *b);
static double island_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
static int start_island(Bar *b) {
    int fd[2]; pid_t child;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fd) < 0) return 0;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC); fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    child = fork();
    if (child == 0) {
        close(fd[0]); close(ConnectionNumber(b->dpy));
        if (b->worker_fd >= 0) close(b->worker_fd);
        island_worker(fd[1]);
    }
    close(fd[1]);
    if (child < 0) { close(fd[0]); return 0; }
    fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
    b->island_fd = fd[0]; return 1;
}
static void island_request(Bar *b, int action, int id) {
    IslandRequest request = {action, id};
    if (b->island_fd >= 0 && send(b->island_fd, &request, sizeof request, MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof request)
        fprintf(stderr, "cui-bar: could not queue island action\n");
}
static void island_action(Bar *b, int action) { island_request(b, action, 0); }
static void next_keyboard_layout(Bar *b) {
    XkbStateRec state; XkbDescPtr keyboard = XkbGetMap(b->dpy, 0, XkbUseCoreKbd);
    if (!keyboard) return;
    if (XkbGetControls(b->dpy, XkbAllControlsMask, keyboard) == Success && keyboard->ctrls && keyboard->ctrls->num_groups > 1 &&
        XkbGetState(b->dpy, XkbUseCoreKbd, &state) == Success)
        XkbLockGroup(b->dpy, XkbUseCoreKbd, (state.group + 1) % keyboard->ctrls->num_groups);
    XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
    XSync(b->dpy, False); read_stats(b);
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
/* Shape the actual X window, not just the pixels, so corners show the desktop. */
static void shape_window(Bar *b, Window win, int width, int height, int radius) {
    typedef int (*Query)(Display *, int *, int *);
    typedef void (*Combine)(Display *, Window, int, int, int, Pixmap, int);
    static void *lib;
    if (!lib) lib = dlopen("libXext.so.6", RTLD_LAZY | RTLD_LOCAL);
    Query query; Combine combine; int event, error; Pixmap mask; GC gc;
    if (!lib) return;
    *(void **)(&query) = dlsym(lib, "XShapeQueryExtension");
    *(void **)(&combine) = dlsym(lib, "XShapeCombineMask");
    if (!query || !combine || !query(b->dpy, &event, &error)) { return; }
    if (width < radius * 2) radius = width / 2;
    if (height < radius * 2) radius = height / 2;
    mask = XCreatePixmap(b->dpy, win, width, height, 1);
    gc = XCreateGC(b->dpy, mask, 0, NULL);
    XSetForeground(b->dpy, gc, 0); XFillRectangle(b->dpy, mask, gc, 0, 0, width, height);
    XSetForeground(b->dpy, gc, 1);
    XFillRectangle(b->dpy, mask, gc, radius, 0, width - radius * 2, height);
    XFillRectangle(b->dpy, mask, gc, 0, radius, width, height - radius * 2);
    XFillArc(b->dpy, mask, gc, 0, 0, radius * 2, radius * 2, 0, 360 * 64);
    XFillArc(b->dpy, mask, gc, width - radius * 2, 0, radius * 2, radius * 2, 0, 360 * 64);
    XFillArc(b->dpy, mask, gc, 0, height - radius * 2, radius * 2, radius * 2, 0, 360 * 64);
    XFillArc(b->dpy, mask, gc, width - radius * 2, height - radius * 2, radius * 2, radius * 2, 0, 360 * 64);
    combine(b->dpy, win, 0 /* ShapeBounding */, 0, 0, mask, 0 /* ShapeSet */);
    XFreeGC(b->dpy, gc); XFreePixmap(b->dpy, mask);
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
static void close_popup(Bar *b);
static void close_tooltip(Bar *b);
/* Accept the old geometry-only protocol too, so upgrading the bar first is safe. */
static int workspace_header(const unsigned long *v, unsigned long n, int *count, int *stride) {
    if (n < 3 || (v[0] != 2 && v[0] != 3)) return 0;
    *stride = v[0] == 3 ? 9 : 8;
    if (v[1] < 1 || v[1] > 16 || n < 3 + v[1] * (unsigned)*stride) return 0;
    *count = (int)v[1]; return 1;
}
static void read_workspace(Bar *b) {
    Atom type; int format; unsigned long n, left; unsigned char *data = NULL;
    if (XGetWindowProperty(b->dpy, b->root, b->state_atom, 0, 3 + 16 * 9, False, XA_CARDINAL,
                           &type, &format, &n, &left, &data) == Success && data &&
        type == XA_CARDINAL && format == 32) {
        unsigned long *v = (unsigned long *)data;
        int count, stride, i;
        if (!workspace_header(v, n, &count, &stride)) goto done;
        b->tag_count = v[2] > 9 ? 9 : (int)v[2];
        for (i = 0; i < count; i++) {
            int at = 3 + i * stride, old_id = b->panels[i].id;
            Panel *p = &b->panels[i];
            int is_new = i >= b->panel_count;
            int was_uninitialized = p->width <= 1;
            int old_x = p->x, old_y = p->y, old_width = p->width;
            if (is_new) {
                XSetWindowAttributes attr;
                p->mapped = 0;
                attr.override_redirect = True; attr.background_pixel = BlackPixel(b->dpy, b->screen);
                attr.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask;
                p->win = XCreateWindow(b->dpy, b->root, 0, 0, 1, HEIGHT, 0, DefaultDepth(b->dpy, b->screen),
                                       InputOutput, DefaultVisual(b->dpy, b->screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
            }
            p->id = (int)v[at]; p->fullscreen = stride == 9 && (v[at + 8] & 1);
            p->active = v[at + 1]; p->occupied = v[at + 2]; p->urgent = v[at + 3];
            p->x = (int32_t)v[at + 4]; p->y = (int32_t)v[at + 5] + TOP_GAP;
            p->width = (int)v[at + 6]; p->height = (int)v[at + 7] - TOP_GAP;
            if (p->width > SIDE_GAP * 2 + 1) { p->x += SIDE_GAP; p->width -= SIDE_GAP * 2; }
            if (p->width <= 0 || p->width > 32767) p->width = 1;
            XMoveResizeWindow(b->dpy, p->win, p->x, p->y, p->width, HEIGHT);
            if (is_new || old_width != p->width) shape_window(b, p->win, p->width, HEIGHT, RADIUS);
            if (p->fullscreen) {
                if (b->popup && b->popup_monitor == p->id) close_popup(b);
                if (b->tooltip && b->tooltip_panel == i) close_tooltip(b);
                if (p->mapped) { XUnmapWindow(b->dpy, p->win); p->mapped = 0; }
            } else if (!p->mapped) {
                XMapRaised(b->dpy, p->win); p->mapped = 1;
            }
            if (is_new || old_id != p->id || was_uninitialized || old_x != p->x || old_y != p->y || old_width != p->width) send_command(b, 2, (long)p->win, p->id);
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
/* Prefer actual CPU hwmon sensors over unrelated thermal zones. */
static int cpu_temperature(void) {
    const char *override = getenv("CUI_BAR_TEMP_PATH");
    const char *cpu_names[] = {"coretemp", "k10temp", "zenpower", "cpu", "x86_pkg_temp", "soc_thermal", NULL};
    glob_t g = {0}; char path[PATH_MAX], name[128]; int i, j, best = -1;
    if (override && *override) {
        int t = read_int(override); return t > 1000 && t < 150000 ? t / 1000 : t >= 0 && t < 150 ? t : -1;
    }
    if (!glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g)) {
        for (i = 0; i < (int)g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r"); char *slash; glob_t sensors = {0};
            if (!f) continue;
            if (!fgets(name, sizeof name, f)) { fclose(f); continue; } fclose(f);
            for (j = 0; cpu_names[j] && !strstr(name, cpu_names[j]); j++) ;
            if (!cpu_names[j]) continue;
            snprintf(path, sizeof path, "%s", g.gl_pathv[i]); slash = strrchr(path, '/');
            if (!slash || (size_t)(slash - path) + sizeof "/temp*_input" > sizeof path) continue;
            strcpy(slash, "/temp*_input");
            if (!glob(path, 0, NULL, &sensors)) {
                size_t k;
                for (k = 0; k < sensors.gl_pathc; k++) {
                    int v = read_int(sensors.gl_pathv[k]);
                    if (v >= 1000 && v < 150000 && v / 1000 > best) best = v / 1000;
                }
            }
            globfree(&sensors);
        }
    }
    globfree(&g); if (best >= 0) return best;
    memset(&g, 0, sizeof g);
    if (!glob("/sys/class/thermal/thermal_zone*/type", 0, NULL, &g)) {
        for (i = 0; i < (int)g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r"); char *slash; int v;
            if (!f) continue;
            if (!fgets(name, sizeof name, f)) { fclose(f); continue; } fclose(f);
            for (j = 0; cpu_names[j] && !strstr(name, cpu_names[j]); j++) ;
            if (!cpu_names[j]) continue;
            snprintf(path, sizeof path, "%s", g.gl_pathv[i]); slash = strrchr(path, '/');
            if (!slash) continue;
            strcpy(slash, "/temp"); v = read_int(path);
            if (v >= 1000 && v < 150000 && v / 1000 > best) best = v / 1000;
        }
    }
    globfree(&g); return best;
}
static void read_cpu_stream(Bar *b, FILE *f) {
    char line[256]; unsigned long long v[8] = {0}, total = 0, idle; int i;
    if (fgets(line, sizeof line, f) && sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
        for (i = 0; i < 8; i++) total += v[i];
        idle = v[3] + v[4];
        if (b->cpu_total && total > b->cpu_total && idle >= b->cpu_idle &&
            idle - b->cpu_idle <= total - b->cpu_total)
            b->cpu = (int)(100 * ((total - b->cpu_total) - (idle - b->cpu_idle)) / (total - b->cpu_total));
        b->cpu_total = total; b->cpu_idle = idle;
    }
    b->core_count = b->core_seen = 0;
    while (fgets(line, sizeof line, f) && !strncmp(line, "cpu", 3)) {
        int id, count; unsigned long long fields[8] = {0}, sum = 0, core_idle; int k, slot;
        if (!isdigit((unsigned char)line[3])) continue;
        count = sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &id,
                       &fields[0], &fields[1], &fields[2], &fields[3], &fields[4], &fields[5], &fields[6], &fields[7]);
        if (count < 5 || id < 0) continue;
        b->core_seen++;
        if (id >= MAX_CPU_THREADS) continue;
        for (k = 0; k < 8; k++) sum += fields[k];
        core_idle = fields[3] + fields[4]; slot = b->core_count++;
        b->core_id[slot] = id; b->core_load[slot] = -1;
        if (b->core_total[id] && sum > b->core_total[id] && core_idle >= b->core_idle[id] &&
            core_idle - b->core_idle[id] <= sum - b->core_total[id])
            b->core_load[slot] = (int)(100 * ((sum - b->core_total[id]) - (core_idle - b->core_idle[id])) / (sum - b->core_total[id]));
        b->core_total[id] = sum; b->core_idle[id] = core_idle;
    }
}
static void read_stats(Bar *b) {
    FILE *f; char line[256]; int i;
    unsigned long mem_total = 0, mem_available = 0;
    glob_t paths = {0}; time_t now = time(NULL); struct tm tm;
    localtime_r(&now, &tm); strftime(b->clock, sizeof b->clock, "%H:%M", &tm);
    f = fopen("/proc/stat", "r");
    if (f) { read_cpu_stream(b, f); fclose(f); }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %lu", &mem_total);
            sscanf(line, "MemAvailable: %lu", &mem_available);
        }
        fclose(f);
        if (mem_total && mem_available <= mem_total) {
            b->memory = (int)(100 * (mem_total - mem_available) / mem_total);
            b->memory_total_kb = mem_total; b->memory_used_kb = mem_total - mem_available;
        }
    }
    b->temperature = cpu_temperature(); b->battery = -1;
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
    if (b->popup_ui) {
        int i;
        for (i = 0; i < ISLAND_NOTICES; i++) {
            if (b->notice_images[i].texture) cui_image_destroy(b->popup_ui, b->notice_images[i].texture);
        }
        memset(b->notice_images, 0, sizeof b->notice_images);
        cui_destroy(b->popup_ui); b->popup_ui = NULL;
    }
    wipe(&popup_state, sizeof popup_state); popup_bar = NULL;
}
static int notification_popup_x(const Bar *b, int width);
static void open_popup(Bar *b, int page) {
    XSetWindowAttributes attr; cui_renderer_iface iface;
    Panel *p = &b->panels[b->current];
    int max_width = page == 4 ? 360 : page == 6 ? 400 : 490;
    int max_height = page == 4 ? 340 : page == 6 ? 480 : 650;
    int width = p->width < max_width ? p->width : max_width;
    int height = p->height - HEIGHT - 12;
    if (height > max_height) height = max_height;
    if (height < 100 || width < 100) return;
    if (b->popup) { close_popup(b); }
    attr.override_redirect = True; attr.background_pixel = BlackPixel(b->dpy, b->screen);
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                      LeaveWindowMask | KeyPressMask | KeyReleaseMask;
    b->popup = XCreateWindow(b->dpy, b->root, p->x + (page == 6 ? notification_popup_x(b, width) : p->width - width), p->y + HEIGHT + 6,
                            width, height, 0, DefaultDepth(b->dpy, b->screen), InputOutput,
                            DefaultVisual(b->dpy, b->screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
    if (!b->popup) return;
    shape_window(b, b->popup, width, height, 14);
    iface = cui_sw_iface(b->renderer);
    b->popup_ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = CUI_THEME_DARK });
    if (!b->popup_ui) { XDestroyWindow(b->dpy, b->popup); b->popup = None; return; }
    style(b->popup_ui);
    cui_font_set_role(b->popup_ui, CUI_FONT_BODY, CUI_FONT_MONO);
    cui_font_set_role(b->popup_ui, CUI_FONT_MEDIUM, CUI_FONT_MONO);
    b->popup_width = width; b->popup_height = height; b->popup_monitor = p->id;
    memset(&popup_state, 0, sizeof popup_state); popup_state.page = page == 5 ? 4 : page;
    popup_bar = b; b->popup_dirty = 1; b->calendar_offset = 0; b->popup_calendar = page == 4; b->popup_notifications = page == 6;
    XMapRaised(b->dpy, b->popup); XSync(b->dpy, False);
    if (XGrabKeyboard(b->dpy, b->popup, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
        fprintf(stderr, "cui-bar: cannot grab keyboard for control center\n"); close_popup(b); return;
    }
    cui_input_focus(b->popup_ui, true);
}
static void calendar_page(cui_ctx *ui, Bar *b) {
    static const char *weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    time_t now = time(NULL); struct tm today, month, next; char title[80], number[8];
    int start, days, row, col, cell = (b->popup_width - 64) / 7;
    localtime_r(&now, &today); month = today; month.tm_mday = 1;
    month.tm_mon += b->calendar_offset; month.tm_hour = 12; month.tm_isdst = -1; mktime(&month);
    next = month; next.tm_mon++; next.tm_mday = 0; next.tm_isdst = -1; mktime(&next);
    days = next.tm_mday; start = (month.tm_wday + 6) % 7;
    if (cell < 18) cell = 18;
    strftime(title, sizeof title, "%B %Y", &month);
    cui_card(ui, .width = CUI_FILL, .pad = 12, .gap = 7) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 6) {
            if (cui_button(ui, "<", .size = CUI_SIZE_SM)) b->calendar_offset--;
            cui_column(ui, .width = CUI_FILL) { cui_text(ui, title, .font = CUI_FONT_MEDIUM, .size = 16); }
            if (cui_button(ui, "Today", .size = CUI_SIZE_SM)) b->calendar_offset = 0;
            if (cui_button(ui, ">", .size = CUI_SIZE_SM)) b->calendar_offset++;
        }
        cui_row(ui, .gap = 0) {
            for (col = 0; col < 7; col++) cui_column(ui, .width = CUI_PX(cell)) {
                cui_text(ui, weekdays[col], .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }
        for (row = 0; row < 6 && row * 7 < start + days; row++) {
            cui_row(ui, .gap = 0) {
                for (col = 0; col < 7; col++) {
                    int day = row * 7 + col - start + 1;
                    cui_column(ui, .width = CUI_PX(cell), .height = CUI_PX(22)) {
                        if (day >= 1 && day <= days) {
                            snprintf(number, sizeof number, "%d", day);
                            if (day == today.tm_mday && month.tm_mon == today.tm_mon && month.tm_year == today.tm_year)
                                cui_text(ui, number, .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_ACCENT);
                            else cui_text(ui, number);
                        }
                    }
                }
            }
        }
    }
}
/* Dunst history can expose a file path, but not raw image-data. Cache each
 * decoded image for the lifetime of the popup rather than loading every frame. */
static cui_texture notice_image(cui_ctx *ui, Bar *b, int i, const char *path) {
    struct stat st;
    if (strcmp(b->notice_images[i].path, path)) {
        if (b->notice_images[i].texture) cui_image_destroy(ui, b->notice_images[i].texture);
        b->notice_images[i].texture = 0;
        copystr(b->notice_images[i].path, sizeof b->notice_images[i].path, path);
        if (path[0] == '/' && !stat(path, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 2 * 1024 * 1024)
            b->notice_images[i].texture = cui_image_load(ui, path, NULL, NULL);
    }
    return b->notice_images[i].texture;
}
static void notifications_page(cui_ctx *ui, Bar *b) {
    int i;
    cui_scroll(ui, .id = "notifications", .width = CUI_FILL, .height = CUI_FILL, .pad = 14, .gap = 10) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_column(ui, .width = CUI_FILL, .gap = 3) {
                cui_text(ui, "Notifications", .font = CUI_FONT_MEDIUM, .size = 18);
                cui_secondary(ui, "Recent history from Dunst");
            }
            if (cui_button(ui, "Close", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) b->popup_page = -1;
        }
        if (b->island.player_available) cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 8) {
            cui_text(ui, b->island.title, .font = CUI_FONT_MEDIUM, .size = 14);
            if (b->island.artist[0]) cui_secondary(ui, b->island.artist);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Previous", .icon = CUI_ICON_SKIP_BACK, .size = CUI_SIZE_SM)) island_action(b, ISLAND_PREVIOUS);
                if (cui_button(ui, b->island.playing ? "Pause" : "Play", .icon = b->island.playing ? CUI_ICON_PAUSE : CUI_ICON_PLAY, .size = CUI_SIZE_SM)) island_action(b, ISLAND_TOGGLE);
                if (cui_button(ui, "Next", .icon = CUI_ICON_SKIP_FORWARD, .size = CUI_SIZE_SM)) island_action(b, ISLAND_NEXT);
            }
        }
        if (!b->island.notifications_available) cui_card(ui, .width = CUI_FILL, .pad = 14) {
            cui_secondary(ui, "Dunst history unavailable. Install and run dunst with dunstctl.");
        }
        else if (!b->island.history_count) cui_card(ui, .width = CUI_FILL, .pad = 14) {
            cui_secondary(ui, "No notifications in history yet.");
        }
        else if (!b->island.notice_count) cui_card(ui, .width = CUI_FILL, .pad = 14) {
            cui_secondary(ui, "History details are unavailable; check dunstctl and busctl.");
        }
        for (i = 0; i < b->island.notice_count; i++) {
            const IslandNotice *n = &b->island.notices[i];
            cui_push_id_int(ui, n->id ? n->id : -(i + 1));
            cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 5) {
                cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                    cui_avatar(ui, n->title[0] ? n->title : n->app, .size = 32,
                               .image = notice_image(ui, b, i, n->icon_path));
                    cui_column(ui, .width = CUI_FILL) {
                        cui_text(ui, n->app[0] ? n->app : "Notification", .size = 10, .color = CUI_COLOR_INFO);
                    }
                    if (cui_button(ui, "Dismiss", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST, .color = CUI_RGB(0xb86a6a), .disabled = !n->id || b->island_fd < 0))
                        island_request(b, ISLAND_DISMISS, n->id);
                }
                cui_text(ui, n->title[0] ? n->title : "Notification", .font = CUI_FONT_MEDIUM, .size = 14);
                if (n->body[0]) cui_text(ui, n->body, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_pop_id(ui);
        }
    }
}
static int draw_popup(Bar *b) {
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
    if (b->popup_calendar) {
        cui_scroll(ui, .id = "calendar", .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 8) {
            calendar_page(ui, b);
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_column(ui, .width = CUI_FILL) { cui_secondary(ui, "Local time"); }
                if (cui_button(ui, "Close", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) b->popup_page = -1;
            }
        }
    } else if (b->popup_notifications) {
        notifications_page(ui, b);
    } else {
        if (dashboard_shell(ui, &popup_state, &b->snapshot, &b->desktop, b->worker_busy)) b->popup_page = -1;
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
typedef struct { int stats, tags, audio, dashboard, network, network_width, bluetooth, tray, clock, language, lock, island, island_width; } Layout;
static Layout layout(const Bar *b) {
    Layout l; int right = b->width - PAD;
    l.lock = right - 28; right = l.lock - 4;
    l.clock = right - 64; right = l.clock - 4;
    l.language = right - 34; right = l.language - 4;
    l.bluetooth = right - 54; right = l.bluetooth - 4;
    l.network_width = NET_WIDTH(b->width);
    l.network = right - l.network_width; right = l.network - 4;
    l.audio = right - 62; right = l.audio - 4;
    l.dashboard = right - 32; right = l.dashboard - 4;
    l.tray = right - (b->current == 0 ? b->tray_width : 0);
    l.tags = 10;
    {
        int left = l.tags + b->tag_count * 28 + 8, right_limit = l.tray - 6;
        l.stats = right_limit - left >= 470;
        if (l.stats) left = l.tags + b->tag_count * 28 + 22 + 296;
        l.island_width = right_limit - left;
        if (l.island_width > 320) l.island_width = 320;
        if (l.island_width < 28) l.island_width = 0;
        l.island = (b->width - l.island_width) / 2;
        if (l.island < left) l.island = left;
        if (l.island + l.island_width > right_limit) l.island = right_limit - l.island_width;
    }
    return l;
}
/* Align the notification panel with the island, clamped to this monitor.
 * If the island does not fit, the dashboard shortcut remains right-aligned. */
static int notification_popup_x(const Bar *b, int width) {
    Layout l = layout(b);
    int x = l.island_width ? l.island + l.island_width / 2 - width / 2 : b->width - width;
    if (x < 0) x = 0;
    if (x > b->width - width) x = b->width - width;
    return x;
}
static void label(cui_ctx *ui, int x, int y, int size, cui_color color, const char *text) {
    cui_draw_text(ui, CUI_FONT_MONO, size, (cui_vec2){x, y}, color, text, -1);
}
static void icon(cui_ctx *ui, int x, int y, int size, cui_color color, const char *glyph) {
    cui_draw_text(ui, CUI_FONT_ICONS, size, (cui_vec2){x, y}, color, glyph, -1);
}
/* One-line controls share a single dock surface, while keeping separate hit areas. */
static void dock_cell(cui_ctx *ui, int x, int w, const char *glyph, const char *value, cui_color accent) {
    char fitted[256]; size_t len;
    icon(ui, x + 6, 7, 13, accent, glyph);
    cui_push_clip(ui, (cui_rect){x + 23, 4, w - 25, HEIGHT - 8});
    snprintf(fitted, sizeof fitted, "%s", value);
    len = strlen(fitted);
    if (cui_measure_text(ui, CUI_FONT_MONO, 11, fitted, -1).width > w - 25) {
        while (len > 0 && cui_measure_text(ui, CUI_FONT_MONO, 11, fitted, -1).width +
               cui_measure_text(ui, CUI_FONT_MONO, 11, "...", -1).width > w - 25) {
            do { len--; } while (len > 0 && ((unsigned char)fitted[len] & 0xc0) == 0x80);
            fitted[len] = 0;
        }
        if (len + 3 < sizeof fitted) strcat(fitted, "...");
    }
    label(ui, x + 23, 8, 11, CUI_RGB(0xadbac7), fitted);
    cui_pop_clip(ui);
}
static void divider(cui_ctx *ui, int x) {
    cui_draw_rect(ui, (cui_rect){x, 8, 1, 12}, CUI_CORNERS(0), CUI_RGB(0x2d3035));
}
/* Prefer the active Wi-Fi SSID even when a wired link is also present.
 * Keep the string intact; the drawing clip handles long and UTF-8 names. */
static const char *network_label(const Bar *b) {
    int i;
    if (!b->network_available) return "--";
    if (b->snapshot.wifi_device[0]) {
        for (i = 0; i < b->snapshot.networks_count; i++)
            if (b->snapshot.networks[i].active && b->snapshot.networks[i].ssid[0]) return b->snapshot.networks[i].ssid;
        return "Connected";
    }
    return b->ethernet ? "Ethernet" : "Offline";
}
/* Travel in both directions, with eased ends and pauses instead of a jump. */
static float island_marquee_offset(float overflow, double now) {
    double travel, phase, t;
    if (overflow <= 0) return 0;
    travel = overflow / 22.0;
    phase = fmod(now, 2.0 * travel + 2.0);
    if (phase < 1.0) return 0;
    phase -= 1.0;
    if (phase < travel) t = phase / travel;
    else if ((phase -= travel) < 1.0) return overflow;
    else t = 1.0 - (phase - 1.0) / travel;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (float)(overflow * t * t * (3.0 - 2.0 * t));
}
/* Center island stays inside the free interval between the rails. */
static void paint_island(cui_ctx *ui, const Bar *b, Layout l) {
    int x = l.island, w = l.island_width, text_left, text_width, controls;
    char text[512]; cui_color white = CUI_RGB(0xe9eff6), muted = CUI_RGB(0x8a949e);
    double now = island_now();
    if (!w) return;
    cui_draw_rect_border(ui, (cui_rect){x, 2, w, 24}, CUI_CORNERS(12), CUI_RGB(0x181a1e), CUI_RGB(0x2d3035), 1);
    icon(ui, x + 7, 7, 13, b->island.notifications_available ? white : muted, CUI_ICON_BELL);
    if (b->island.notifications_available && (b->island.active_count || b->island.history_count) && w >= 120) {
        int count = b->island.active_count ? b->island.active_count : b->island.history_count;
        snprintf(text, sizeof text, "%d", count > 99 ? 99 : count);
        label(ui, x + 23, 8, 10, white, text);
    }
    if (w < 100) return;
    controls = x + w - 62;
    icon(ui, controls, 8, 12, b->island.player_available ? white : muted, CUI_ICON_SKIP_BACK);
    icon(ui, controls + 20, 7, 14, b->island.player_available ? white : muted, b->island.playing ? CUI_ICON_PAUSE : CUI_ICON_PLAY);
    icon(ui, controls + 41, 8, 12, b->island.player_available ? white : muted, CUI_ICON_SKIP_FORWARD);
    text_left = x + (w >= 120 ? 45 : 27);
    if (w >= 160 && b->island.player_available && b->island.playing) {
        int i, tick = (int)(now * 8);
        for (i = 0; i < 3; i++) {
            int h = 3 + ((tick + i * 3) % 5);
            cui_draw_rect(ui, (cui_rect){text_left + i * 3, 18 - h, 2, h}, CUI_CORNERS(1), white);
        }
        text_left += 13;
    }
    text_width = controls - text_left - 6;
    if (text_width < 12) return;
    if (b->island_notice_until > now && b->island_live) snprintf(text, sizeof text, "New notification");
    else if (b->island_notice_until > now && b->island.notice_count)
        snprintf(text, sizeof text, "%s: %s", b->island.notices[0].app[0] ? b->island.notices[0].app : "Notification", b->island.notices[0].title);
    else if (b->island.player_available)
        snprintf(text, sizeof text, "%s%s%s", b->island.artist, b->island.artist[0] ? "  /  " : "", b->island.title);
    else snprintf(text, sizeof text, "%s", b->island.notifications_available ? "No music playing" : "No player / Dunst");
    {
        float full = cui_measure_text(ui, CUI_FONT_MONO, 11, text, -1).width;
        float offset = 0;
        if (full > text_width && (b->island.playing || b->island_notice_until > now))
            offset = island_marquee_offset(full - text_width, now);
        cui_push_clip(ui, (cui_rect){text_left, 4, text_width, HEIGHT - 8});
        cui_draw_text(ui, CUI_FONT_MONO, 11, (cui_vec2){text_left - offset, 8}, white, text, -1);
        cui_pop_clip(ui);
    }
}
static void paint(cui_ctx *ui, cui_rect r, void *user) {
    Bar *b = *(Bar **)user; Layout l = layout(b); int i; char text[160];
    cui_color fg = CUI_RGB(0xadbac7), muted = CUI_RGB(0x8a949e), blue = CUI_RGB(0x7fa8c9), white = CUI_RGB(0xe9eff6);
    (void)r;
    cui_draw_rect_border(ui, (cui_rect){0, 0, (float)b->width, HEIGHT}, CUI_CORNERS(RADIUS), CUI_RGB(0x090b0d), CUI_RGB(0x2d3035), 1);
    if (l.tags + b->tag_count * 28 <= l.tray - 4) {
        /* A single workspace rail, with a clear selected capsule and occupancy dots. */
        cui_draw_rect(ui, (cui_rect){l.tags - 3, 3, b->tag_count * 28 + 3, 22}, CUI_CORNERS(8), CUI_RGB(0x121417));
        for (i = 0; i < b->tag_count; i++) {
            int x = l.tags + i * 28; unsigned long mask = 1ul << i;
            if (b->active & mask) cui_draw_rect(ui, (cui_rect){x + 2, 5, 23, 18}, CUI_CORNERS(7), CUI_RGB(0x2d3035));
            else if (b->occupied & mask) cui_draw_circle(ui, (cui_vec2){x + 14, 22}, 1.3f, blue);
            snprintf(text, sizeof text, "%d", i + 1);
            label(ui, x + 10, 7, 11, b->urgent & mask ? CUI_RGB(0xb86a6a) : b->active & mask ? white : b->occupied & mask ? fg : muted, text);
        }
    }
    if (l.stats) {
        int x = l.tags + b->tag_count * 28 + 22;
        /* Telemetry reads like a compact instrument strip rather than stacked labels. */
        cui_draw_rect(ui, (cui_rect){x - 5, 3, 295, 22}, CUI_CORNERS(8), CUI_RGB(0x121417));
        icon(ui, x + 3, 8, 12, blue, CUI_ICON_CPU);
        if (b->cpu >= 0) snprintf(text, sizeof text, "%d%%", b->cpu); else strcpy(text, "--");
        label(ui, x + 20, 8, 11, fg, text); divider(ui, x + 69);
        icon(ui, x + 78, 8, 12, blue, CUI_ICON_DATABASE);
        if (b->memory_total_kb) snprintf(text, sizeof text, "%.1fG", b->memory_used_kb / 1048576.0); else strcpy(text, "--");
        label(ui, x + 95, 8, 11, fg, text); divider(ui, x + 144);
        icon(ui, x + 153, 8, 12, blue, CUI_ICON_ACTIVITY);
        if (b->temperature >= 0) snprintf(text, sizeof text, "%d C", b->temperature); else strcpy(text, "-- C");
        label(ui, x + 170, 8, 11, fg, text);
        if (b->battery >= 0) {
            divider(ui, x + 219); icon(ui, x + 228, 8, 12, blue, CUI_ICON_BATTERY);
            snprintf(text, sizeof text, "%d%%", b->battery);
            label(ui, x + 245, 8, 11, b->battery <= 15 ? CUI_RGB(0xb86a6a) : fg, text);
        }
    }
    paint_island(ui, b, l);
    /* A unified status dock with subtle separators keeps click targets distinct. */
    cui_draw_rect(ui, (cui_rect){l.dashboard - 2, 3, l.lock + 28 - l.dashboard + 2, 22}, CUI_CORNERS(8), CUI_RGB(0x121417));
    icon(ui, l.dashboard + 9, 7, 14, blue, CUI_ICON_LAYOUT_DASHBOARD);
    divider(ui, l.audio - 2); divider(ui, l.network - 2); divider(ui, l.bluetooth - 2);
    divider(ui, l.language - 2); divider(ui, l.clock - 2); divider(ui, l.lock - 2);
    if (b->snapshot.audio_available) { if (b->snapshot.muted) strcpy(text, "MUTE"); else snprintf(text, sizeof text, "%d%%", b->snapshot.volume); } else strcpy(text, "--");
    dock_cell(ui, l.audio, 62, b->snapshot.muted ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, text, blue);
    dock_cell(ui, l.network, l.network_width, b->snapshot.wifi_device[0] ? CUI_ICON_WIFI : CUI_ICON_GLOBE, network_label(b),
              b->ethernet || b->snapshot.wifi_device[0] ? blue : muted);
    if (!b->snapshot.bluetooth_available) strcpy(text, "--"); else if (!b->snapshot.bluetooth_enabled) strcpy(text, "OFF");
    else { int connected = 0; for (i = 0; i < b->snapshot.devices_count; i++) connected += b->snapshot.devices[i].connected;
        if (connected) snprintf(text, sizeof text, "%d", connected); else strcpy(text, "ON"); }
    dock_cell(ui, l.bluetooth, 54, CUI_ICON_LINK, text, b->snapshot.bluetooth_enabled ? blue : muted);
    label(ui, l.language + 5, 8, 11, muted, b->language);
    icon(ui, l.clock + 5, 8, 12, blue, CUI_ICON_CLOCK);
    label(ui, l.clock + 22, 8, 11, white, b->clock);
    icon(ui, l.lock + 7, 7, 14, muted, CUI_ICON_LOCK);
}
enum { TIP_NONE, TIP_CPU, TIP_MEMORY, TIP_TEMP, TIP_BATTERY, TIP_SOUND, TIP_NETWORK, TIP_BT, TIP_CLOCK, TIP_LANGUAGE, TIP_LOCK, TIP_DASHBOARD, TIP_ISLAND };
static int hover_kind(const Bar *b, int x) {
    Layout l = layout(b); int sx = l.tags + b->tag_count * 28 + 22;
    if (l.stats && x >= sx && x < sx + 75) return TIP_CPU;
    if (l.stats && x >= sx + 75 && x < sx + 150) return TIP_MEMORY;
    if (l.stats && x >= sx + 150 && x < sx + 225) return TIP_TEMP;
    if (l.stats && b->battery >= 0 && x >= sx + 225 && x < sx + 295) return TIP_BATTERY;
    if (l.island_width && x >= l.island && x < l.island + l.island_width) return TIP_ISLAND;
    if (x >= l.dashboard && x < l.dashboard + 32) return TIP_DASHBOARD;
    if (x >= l.audio && x < l.audio + 62) return TIP_SOUND;
    if (x >= l.network && x < l.network + l.network_width) return TIP_NETWORK;
    if (x >= l.bluetooth && x < l.bluetooth + 54) return TIP_BT;
    if (x >= l.clock && x < l.clock + 64) return TIP_CLOCK;
    if (x >= l.language && x < l.language + 34) return TIP_LANGUAGE;
    if (x >= l.lock && x < l.lock + 28) return TIP_LOCK;
    return TIP_NONE;
}
static void close_tooltip(Bar *b) {
    if (b->tooltip) XDestroyWindow(b->dpy, b->tooltip);
    b->tooltip = None; b->tooltip_kind = TIP_NONE;
    if (b->tooltip_ui) cui_destroy(b->tooltip_ui);
    b->tooltip_ui = NULL;
}
static void paint_tooltip(cui_ctx *ui, cui_rect r, void *user) {
    Bar *b = *(Bar **)user; int w = b->tooltip_width, kind = b->tooltip_kind, i;
    const cui_color fg = CUI_RGB(0xadbac7), muted = CUI_RGB(0x8a949e), blue = CUI_RGB(0x7fa8c9);
    char line[256]; const char *glyph;
    (void)r;
    switch (kind) {
    case TIP_CPU: glyph = CUI_ICON_CPU; break;
    case TIP_MEMORY: glyph = CUI_ICON_DATABASE; break;
    case TIP_TEMP: glyph = CUI_ICON_ACTIVITY; break;
    case TIP_BATTERY: glyph = CUI_ICON_BATTERY; break;
    case TIP_SOUND: glyph = b->snapshot.muted ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2; break;
    case TIP_NETWORK: glyph = b->snapshot.wifi_device[0] ? CUI_ICON_WIFI : CUI_ICON_GLOBE; break;
    case TIP_BT: glyph = CUI_ICON_LINK; break;
    case TIP_CLOCK: glyph = CUI_ICON_CLOCK; break;
    case TIP_LOCK: glyph = CUI_ICON_LOCK; break;
    case TIP_DASHBOARD: glyph = CUI_ICON_LAYOUT_DASHBOARD; break;
    case TIP_ISLAND: glyph = CUI_ICON_BELL; break;
    default: glyph = NULL; break;
    }
    /* Square, bar-colored shell with the same inset surface as the bar cells. */
    cui_draw_rect_border(ui, (cui_rect){0, 0, w, b->tooltip_height}, CUI_CORNERS(0), CUI_RGB(0x090b0d), CUI_RGB(0x2d3035), 1);
    cui_draw_rect(ui, (cui_rect){5, 5, w - 10, b->tooltip_height - 10}, CUI_CORNERS(0), CUI_RGB(0x121417));
    if (glyph) icon(ui, 14, 13, 13, blue, glyph);
    if (kind == TIP_CPU) {
        int cols = (w - 24) / 85, limit, rows;
        if (cols < 1) cols = 1;
        limit = cols * 8; if (limit > b->core_count) limit = b->core_count;
        rows = (limit + cols - 1) / cols;
        label(ui, 34, 12, 14, fg, "CPU activity");
        snprintf(line, sizeof line, "Overall: %s", b->cpu >= 0 ? "" : "unavailable");
        if (b->cpu >= 0) snprintf(line, sizeof line, "Overall: %d%%  /  %d logical CPUs", b->cpu, b->core_seen);
        label(ui, 14, 32, 11, muted, line);
        for (i = 0; i < limit; i++) {
            int col = i % cols, row = i / cols;
            if (b->core_load[i] >= 0) snprintf(line, sizeof line, "CPU%-3d %3d%%", b->core_id[i], b->core_load[i]);
            else snprintf(line, sizeof line, "CPU%-3d  --", b->core_id[i]);
            label(ui, 14 + col * ((w - 24) / cols), 52 + row * 17, 11, blue, line);
        }
        if (b->core_seen > limit) { snprintf(line, sizeof line, "%d of %d logical CPUs shown", limit, b->core_seen); label(ui, 14, 55 + rows * 17, 10, muted, line); }
        if (!b->core_count) label(ui, 14, 52, 11, muted, "Per-CPU data unavailable");
    } else if (kind == TIP_MEMORY) {
        label(ui, 34, 12, 14, fg, "Memory");
        if (b->memory_total_kb) snprintf(line, sizeof line, "%.2f GiB used  /  %.2f GiB total", b->memory_used_kb / 1048576.0, b->memory_total_kb / 1048576.0);
        else strcpy(line, "Memory data unavailable");
        label(ui, 14, 36, 11, blue, line);
        label(ui, 14, 55, 10, muted, "Used = total minus available (excludes reclaimable cache).");
    } else {
        const char *title = "", *detail = "";
        switch (kind) {
        case TIP_TEMP: title = "CPU temperature"; if (b->temperature >= 0) snprintf(line, sizeof line, "%d C  /  CPU sensor", b->temperature); else strcpy(line, "No readable CPU temperature sensor"); detail = line; break;
        case TIP_BATTERY: title = "Battery"; snprintf(line, sizeof line, "%d%% remaining", b->battery); detail = line; break;
        case TIP_SOUND: title = "Sound"; if (b->snapshot.audio_available) snprintf(line, sizeof line, "%d%%  /  %s", b->snapshot.volume, b->snapshot.muted ? "Muted" : "Default output"); else strcpy(line, "Audio service unavailable"); detail = line; break;
        case TIP_NETWORK: title = "Network"; detail = network_label(b); break;
        case TIP_BT: title = "Bluetooth"; detail = !b->snapshot.bluetooth_available ? "Controller unavailable" : b->snapshot.bluetooth_enabled ? "On / click to manage devices" : "Off / right-click to enable"; break;
        case TIP_CLOCK: title = "Date & time"; { time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(line, sizeof line, "%A, %d %B %Y  /  %H:%M", &tm); detail = line; } break;
        case TIP_LANGUAGE: title = "Keyboard layout"; snprintf(line, sizeof line, "%s  /  click to cycle layouts", b->language); detail = line; break;
        case TIP_DASHBOARD: title = "Control center"; detail = "Click: dashboard  /  right-click: notifications"; break;
        case TIP_ISLAND: title = "Music & notifications"; if (b->island.notifications_available) snprintf(line, sizeof line, "%d active, %d in history  /  click to view", b->island.active_count, b->island.history_count); else strcpy(line, "Dunst history unavailable  /  click for details"); detail = line; break;
        case TIP_LOCK: title = "Lock screen"; detail = "Click to lock"; break;
        }
        label(ui, glyph ? 34 : 14, 12, 14, fg, title);
        cui_push_clip(ui, (cui_rect){14, 35, w - 28, 20});
        label(ui, 14, 38, 11, blue, detail);
        cui_pop_clip(ui);
    }
}
static int draw_tooltip(Bar *b) {
    XImage *image; Bar *ptr = b;
    if (!b->tooltip || !b->tooltip_ui) return 1;
    image = XCreateImage(b->dpy, DefaultVisual(b->dpy, b->screen), DefaultDepth(b->dpy, b->screen), ZPixmap, 0, NULL,
                         b->tooltip_width, b->tooltip_height, 32, 0);
    if (!image) return 0;
    if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst || image->red_mask != 0xff0000 ||
        image->green_mask != 0xff00 || image->blue_mask != 0xff) { XDestroyImage(image); return 0; }
    image->data = calloc((size_t)image->bytes_per_line, b->tooltip_height);
    if (!image->data) { XDestroyImage(image); return 0; }
    cui_set_viewport(b->tooltip_ui, b->tooltip_width, b->tooltip_height, 1);
    cui_begin_frame(b->tooltip_ui, .016f);
    cui_canvas(b->tooltip_ui, "tooltip", CUI_FILL, CUI_FILL, paint_tooltip, &ptr, sizeof ptr);
    cui_end_frame(b->tooltip_ui);
    cui_sw_render(b->renderer, cui_get_draw_data(b->tooltip_ui), image->data, b->tooltip_width, b->tooltip_height, image->bytes_per_line, CUI_SW_BGRA);
    XPutImage(b->dpy, b->tooltip, b->gc, image, 0, 0, 0, 0, b->tooltip_width, b->tooltip_height);
    XDestroyImage(image); XFlush(b->dpy); return 1;
}
static void hover_panel(Bar *b, int index, int x) {
    Panel *p = &b->panels[index]; int kind, width, height, xpos; XSetWindowAttributes a; cui_renderer_iface iface;
    use_panel(b, index); kind = b->popup ? TIP_NONE : hover_kind(b, x);
    if (kind == b->tooltip_kind && index == b->tooltip_panel) { use_panel(b, 0); return; }
    close_tooltip(b);
    if (kind == TIP_NONE || p->width < 100) { use_panel(b, 0); return; }
    width = p->width < 390 ? p->width : 390;
    if (kind == TIP_CPU) {
        int cols = (width - 24) / 85, limit, rows;
        if (cols < 1) cols = 1;
        limit = cols * 8; if (limit > b->core_count) limit = b->core_count;
        rows = (limit + cols - 1) / cols;
        height = 70 + rows * 17 + (b->core_seen > limit ? 15 : 0);
    } else height = kind == TIP_MEMORY ? 78 : 64;
    if (height > p->height - HEIGHT - 12) height = p->height - HEIGHT - 12;
    if (height < 40) { use_panel(b, 0); return; }
    xpos = x - width / 2; if (xpos < 0) xpos = 0; if (xpos > p->width - width) xpos = p->width - width;
    a.override_redirect = True; a.background_pixel = BlackPixel(b->dpy, b->screen); a.event_mask = ExposureMask;
    b->tooltip = XCreateWindow(b->dpy, b->root, p->x + xpos, p->y + HEIGHT + 6, width, height, 0,
                                DefaultDepth(b->dpy, b->screen), InputOutput, DefaultVisual(b->dpy, b->screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    if (!b->tooltip) { use_panel(b, 0); return; }
    b->tooltip_kind = kind; b->tooltip_panel = index; b->tooltip_width = width; b->tooltip_height = height;
    iface = cui_sw_iface(b->renderer); b->tooltip_ui = cui_create(&(cui_ctx_desc){.renderer = iface, .theme = CUI_THEME_DARK});
    if (!b->tooltip_ui) { close_tooltip(b); use_panel(b, 0); return; }
    XMapRaised(b->dpy, b->tooltip); if (!draw_tooltip(b)) close_tooltip(b);
    use_panel(b, 0);
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
    else if (l.island_width && x >= l.island && x < l.island + l.island_width && e->button == Button1) {
        int relative = x - l.island;
        if (l.island_width >= 100 && relative >= l.island_width - 66) {
            if (b->island.player_available) {
                int button = relative - (l.island_width - 66);
                island_action(b, button < 21 ? ISLAND_PREVIOUS : button < 42 ? ISLAND_TOGGLE : ISLAND_NEXT);
            }
        } else open_popup(b, 6);
    }
    else if (x >= l.dashboard && x < l.dashboard + 32) {
        if (e->button == Button1) open_popup(b, 5);
        else if (e->button == Button3) open_popup(b, 6);
    }
    else if (x >= l.language && x < l.language + 34 && e->button == Button1) next_keyboard_layout(b);
    else if (x >= l.audio && x < l.audio + 62) {
        if (e->button == Button1) open_popup(b, 2);
        else if (e->button == Button3 && b->snapshot.audio_available) audio_action(b, ACT_AUDIO_MUTE, 0);
        else if ((e->button == Button4 || e->button == Button5) && b->snapshot.audio_available) {
            int value = b->snapshot.volume + (e->button == Button4 ? 2 : -2);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            audio_action(b, ACT_AUDIO_VOLUME, value);
        }
    } else if (x >= l.network && x < l.network + l.network_width) {
        if (e->button == Button1) open_popup(b, 0);
        else if (e->button == Button3 && b->snapshot.wifi_available)
            audio_action(b, b->snapshot.wifi_enabled ? ACT_WIFI_OFF : ACT_WIFI_ON, 0);
    } else if (x >= l.bluetooth && x < l.bluetooth + 54) {
        if (e->button == Button1) open_popup(b, 1);
        else if (e->button == Button3 && b->snapshot.bluetooth_available)
            audio_action(b, b->snapshot.bluetooth_enabled ? ACT_BT_OFF : ACT_BT_ON, 0);
    }
    else if (x >= l.clock && x < l.clock + 64 && e->button == Button1) open_popup(b, 4);
    else if (x >= l.lock && e->button == Button1) launch("mel");
}

int main(void) {
    Bar b = {0}; XSetWindowAttributes attr; WorkerFrame incoming; size_t received = 0;
    time_t next_stats = 0; int dirty = 1, i, result = 0;
    cui_renderer_iface iface;
    b.worker_fd = b.island_fd = -1; b.cpu = b.memory = b.temperature = b.battery = -1;
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
    attr.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask | SubstructureNotifyMask | SubstructureRedirectMask;
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
    if (b.panels[0].width <= 1)
        fprintf(stderr, "cui-bar: waiting for dwm monitor IPC; rebuild and restart dwm if it does not appear\n");
    if (!start_worker(&b)) fprintf(stderr, "cui-bar: could not start service worker\n");
    if (!start_island(&b)) fprintf(stderr, "cui-bar: could not start island worker\n");
    while (running) {
        fd_set fds; int xfd = ConnectionNumber(b.dpy), maxfd = xfd, ready;
        struct timeval timeout; time_t now = time(NULL);
        if (now >= next_stats) { read_stats(&b); next_stats = now + 1; dirty = 1; }
        if (dirty) {
            for (i = 0; i < b.panel_count; i++) {
                use_panel(&b, i);
                if (b.panels[i].mapped && !draw(&b)) { fprintf(stderr, "cui-bar: unsupported X11 visual or allocation failure\n"); result = 1; running = 0; break; }
            }
            use_panel(&b, 0);
            if (!running) break;
            dirty = 0;
            if (b.popup) b.popup_dirty = 1;
            if (b.tooltip && !draw_tooltip(&b)) close_tooltip(&b);
        }
        if (b.popup && b.popup_dirty) { if (!draw_popup(&b)) close_popup(&b); b.popup_dirty = 0; }
        FD_ZERO(&fds); FD_SET(xfd, &fds);
        if (b.worker_fd >= 0) { FD_SET(b.worker_fd, &fds); if (b.worker_fd > maxfd) maxfd = b.worker_fd; }
        if (b.island_fd >= 0) { FD_SET(b.island_fd, &fds); if (b.island_fd > maxfd) maxfd = b.island_fd; }
        timeout.tv_sec = next_stats > now ? next_stats - now : 0; timeout.tv_usec = 0;
        if ((b.island.playing || b.island_notice_until > island_now()) && timeout.tv_sec > 0) { timeout.tv_sec = 0; timeout.tv_usec = 33333; }
        else if (b.popup && timeout.tv_sec > 0) { timeout.tv_sec = 0; timeout.tv_usec = 250000; }
        ready = XPending(b.dpy) ? 0 : select(maxfd + 1, &fds, NULL, NULL, &timeout);
        if (ready == 0 && b.popup) b.popup_dirty = 1;
        if (ready == 0 && (b.island.playing || b.island_notice_until > island_now())) dirty = 1;
        if (ready < 0 && errno != EINTR) { fprintf(stderr, "cui-bar: select: %s\n", strerror(errno)); result = 1; break; }
        if (ready > 0 && b.island_fd >= 0 && FD_ISSET(b.island_fd, &fds)) {
            IslandFrame next; ssize_t n = recv(b.island_fd, &next, sizeof next, 0);
            if (n == sizeof next) {
                if (b.island_initialized && next.notifications_available && b.island.notifications_available) {
                    if (next.notice_count && (next.history_count > b.island.history_count ||
                        (b.island.notice_count && (strcmp(next.notices[0].title, b.island.notices[0].title) || strcmp(next.notices[0].app, b.island.notices[0].app))))) {
                        b.island_notice_until = island_now() + 3; b.island_live = 0;
                    } else if (next.active_count > b.island.active_count) {
                        b.island_notice_until = island_now() + 3; b.island_live = 1;
                    }
                }
                b.island = next; b.island_initialized = 1; dirty = 1;
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                close(b.island_fd); b.island_fd = -1; memset(&b.island, 0, sizeof b.island); dirty = 1;
            }
        }
        if (ready > 0 && b.worker_fd >= 0 && FD_ISSET(b.worker_fd, &fds)) {
            ssize_t n = read(b.worker_fd, (char *)&incoming + received, sizeof incoming - received);
            if (n > 0) {
                received += (size_t)n;
                if (received == sizeof incoming) {
                    b.snapshot = incoming.snapshot; b.desktop = incoming.desktop; b.ethernet = incoming.ethernet; b.network_available = incoming.network_available;
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
            if (e.type == Expose && e.xexpose.window == b.tooltip && e.xexpose.count == 0) { if (!draw_tooltip(&b)) close_tooltip(&b); }
            else if (e.type == MotionNotify && (index = panel_index(&b, e.xmotion.window)) >= 0) hover_panel(&b, index, e.xmotion.x);
            else if (e.type == EnterNotify && (index = panel_index(&b, e.xcrossing.window)) >= 0) hover_panel(&b, index, e.xcrossing.x);
            else if (e.type == LeaveNotify && panel_index(&b, e.xcrossing.window) >= 0) close_tooltip(&b);
            else if (e.type == Expose && panel_index(&b, e.xexpose.window) >= 0 && e.xexpose.count == 0) dirty = 1;
            else if (e.type == ButtonPress && (index = panel_index(&b, e.xbutton.window)) >= 0) {
                close_tooltip(&b); use_panel(&b, index); clicked(&b, &e.xbutton); use_panel(&b, 0); dirty = 1;
            }
            else if (e.type == ClientMessage && e.xclient.window == b.win && e.xclient.message_type == b.opcode && e.xclient.data.l[1] == 0) {
                dock(&b, (Window)e.xclient.data.l[2]); dirty = 1;
            } else if (e.type == SelectionClear && e.xselectionclear.selection == b.selection) {
                fprintf(stderr, "cui-bar: system tray ownership lost\n"); running = 0;
            } else if (e.type == DestroyNotify && (index = icon_index(&b, e.xdestroywindow.window)) >= 0) {
                remove_icon(&b, index); dirty = 1;
            } else if (e.type == PropertyNotify && e.xproperty.window == b.root && e.xproperty.atom == b.state_atom) {
                close_tooltip(&b); read_workspace(&b); dirty = 1;
            } else if (e.type == PropertyNotify && e.xproperty.window == b.root && e.xproperty.atom == b.wm_atom) {
                for (i = 0; i < b.panel_count; i++) send_command(&b, 2, (long)b.panels[i].win, b.panels[i].id);
            } else if (e.type == PropertyNotify && (index = icon_index(&b, e.xproperty.window)) >= 0 && e.xproperty.atom == b.xembed_info) {
                int mapped = icon_mapped(&b, b.icons[index].win);
                if (mapped != b.icons[index].mapped) { b.icons[index].mapped = mapped; position_icons(&b); dirty = 1; }
            } else if (e.type == MapRequest && (index = icon_index(&b, e.xmaprequest.window)) >= 0) {
                if (b.icons[index].mapped) XMapWindow(b.dpy, b.icons[index].win);
            } else if (e.type == ResizeRequest && icon_index(&b, e.xresizerequest.window) >= 0) position_icons(&b);
            else if (e.type == ConfigureRequest && icon_index(&b, e.xconfigurerequest.window) >= 0) position_icons(&b);
            else if (e.type == ConfigureNotify && e.xconfigure.window == b.root) { close_tooltip(&b); read_workspace(&b); dirty = 1; }
        }
    }
cleanup:
    close_tooltip(&b);
    close_popup(&b);
    if (b.worker_fd >= 0) close(b.worker_fd);
    if (b.island_fd >= 0) close(b.island_fd);
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
