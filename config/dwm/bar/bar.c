/* A small, hackable GTK3 bar for this dwm configuration (X11 only). */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BAR_HEIGHT 40 /* keep in sync with externalbarheight in config.h */
#define TAG_COUNT 9

static const char *tag_icons[TAG_COUNT] = {
    "\uf121", "\uf269", "\uf120", "\uf1c0", "\uf075",
    "\uf013", "\uf002", "\uf04b", "\uf07b"
};

typedef struct {
    int id, x, y, width, height, seen;
    GtkWidget *window, *cpu, *audio, *network, *clock;
    GtkWidget *tags[TAG_COUNT];
} Bar;

typedef struct { int monitor, tag; } TagAction;
typedef struct { char audio[64], network[64]; } Reading;

static Display *xdpy;
static Window root;
static Atom state_atom, view_atom;
static GList *bars;
static char cpu_text[32] = "CPU --", audio_text[64] = "Audio --", network_text[64] = "Offline";
static long long previous_total, previous_idle;
static gint worker_running;

static void run_command(const char *command) {
    gchar **argv = NULL;
    if (g_shell_parse_argv(command, NULL, &argv, NULL)) {
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_strfreev(argv);
    }
}

static gboolean module_click(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    const char *name = data;
    (void)widget;
    if (event->type != GDK_BUTTON_PRESS) return TRUE;
    if (!strcmp(name, "audio")) {
        if (event->button == 1 || event->button == 3)
            run_command("pactl set-sink-mute @DEFAULT_SINK@ toggle");
        else if (event->button == 2) run_command("pavucontrol");
    } else if (!strcmp(name, "network") && (event->button == 1 || event->button == 3))
        run_command("nm-connection-editor");
    else if (!strcmp(name, "cpu") && event->button == 1) run_command("ghostty -e top");
    else if (!strcmp(name, "clock") && event->button == 1) run_command("gsimplecal");
    return TRUE;
}

static gboolean audio_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data) {
    (void)widget; (void)data;
    if (event->direction == GDK_SCROLL_UP)
        run_command("pactl set-sink-volume @DEFAULT_SINK@ +2%");
    else if (event->direction == GDK_SCROLL_DOWN)
        run_command("pactl set-sink-volume @DEFAULT_SINK@ -2%");
    return TRUE;
}

static gboolean tag_click(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    TagAction *action = data;
    XEvent msg;
    (void)widget;
    if (event->type != GDK_BUTTON_PRESS || (event->button != 1 && event->button != 3)) return TRUE;
    memset(&msg, 0, sizeof msg);
    msg.xclient.type = ClientMessage;
    msg.xclient.window = root;
    msg.xclient.message_type = view_atom;
    msg.xclient.format = 32;
    msg.xclient.data.l[0] = action->monitor;
    msg.xclient.data.l[1] = 1L << action->tag;
    msg.xclient.data.l[2] = event->button == 3;
    XSendEvent(xdpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &msg);
    XFlush(xdpy);
    return TRUE;
}

static GtkWidget *module(const char *name, GtkWidget **label) {
    GtkWidget *button = gtk_button_new();
    *label = gtk_label_new("");
    gtk_container_add(GTK_CONTAINER(button), *label);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "module");
    gtk_style_context_add_class(gtk_widget_get_style_context(button), name);
    gtk_widget_add_events(button, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    g_signal_connect(button, "button-press-event", G_CALLBACK(module_click), (gpointer)name);
    if (!strcmp(name, "audio")) g_signal_connect(button, "scroll-event", G_CALLBACK(audio_scroll), NULL);
    gtk_widget_set_can_focus(button, FALSE);
    return button;
}

static Bar *create_bar(int id, int x, int y, int width, int height) {
    Bar *bar = g_new0(Bar, 1);
    GtkWidget *box, *left, *center, *right, *button;
    int i;
    bar->id = id; bar->x = x; bar->y = y; bar->width = width; bar->height = height;
    bar->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(bar->window, "bar");
    gtk_window_set_decorated(GTK_WINDOW(bar->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(bar->window), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(bar->window), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(bar->window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(bar->window), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_move(GTK_WINDOW(bar->window), x, y);
    gtk_window_set_default_size(GTK_WINDOW(bar->window), width, BAR_HEIGHT);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(center), "workspaces");
    gtk_container_add(GTK_CONTAINER(bar->window), box);
    gtk_box_pack_start(GTK_BOX(box), left, FALSE, FALSE, 0);
    gtk_box_set_center_widget(GTK_BOX(box), center);
    gtk_box_pack_end(GTK_BOX(box), right, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), module("cpu", &bar->cpu), FALSE, FALSE, 0);
    for (i = 0; i < TAG_COUNT; i++) {
        TagAction *action = g_new(TagAction, 1);
        action->monitor = id; action->tag = i;
        button = gtk_button_new_with_label(tag_icons[i]);
        bar->tags[i] = button;
        gtk_widget_set_can_focus(button, FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "tag");
        g_signal_connect(button, "button-press-event", G_CALLBACK(tag_click), action);
        g_object_set_data_full(G_OBJECT(button), "tag-action", action, g_free);
        gtk_box_pack_start(GTK_BOX(center), button, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(right), module("audio", &bar->audio), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), module("network", &bar->network), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), module("clock", &bar->clock), FALSE, FALSE, 0);
    gtk_widget_realize(bar->window);
    gdk_window_set_override_redirect(gtk_widget_get_window(bar->window), TRUE);
    gtk_widget_show_all(bar->window);
    gtk_window_move(GTK_WINDOW(bar->window), x, y);
    gtk_window_resize(GTK_WINDOW(bar->window), width, BAR_HEIGHT);
    return bar;
}

static Bar *find_bar(int id) {
    GList *p;
    for (p = bars; p; p = p->next) if (((Bar *)p->data)->id == id) return p->data;
    return NULL;
}

static void set_class(GtkWidget *widget, const char *name, gboolean enabled) {
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    if (enabled) gtk_style_context_add_class(context, name);
    else gtk_style_context_remove_class(context, name);
}

/* dwm publishes one line per monitor: id x y w h selected occupied urgent. */
static gboolean update_monitors(gpointer unused) {
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *value = NULL;
    char *line, *save;
    GList *p, *next;
    (void)unused;
    if (XGetWindowProperty(xdpy, root, state_atom, 0, 4096, False, XA_STRING,
                           &actual, &format, &count, &remaining, &value) != Success ||
        actual != XA_STRING || format != 8 || !value) {
        if (value) XFree(value);
        for (p = bars; p; p = p->next) gtk_widget_hide(((Bar *)p->data)->window);
        return G_SOURCE_CONTINUE;
    }
    for (p = bars; p; p = p->next) ((Bar *)p->data)->seen = 0;
    for (line = strtok_r((char *)value, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int id, x, y, w, h, i;
        unsigned int selected, occupied, urgent;
        Bar *bar;
        if (sscanf(line, "%d %d %d %d %d %u %u %u", &id, &x, &y, &w, &h,
                   &selected, &occupied, &urgent) != 8 || w <= 0 || h <= 0) continue;
        bar = find_bar(id);
        if (bar && (bar->x != x || bar->y != y || bar->width != w || bar->height != h)) {
            bars = g_list_remove(bars, bar); gtk_widget_destroy(bar->window); g_free(bar); bar = NULL;
        }
        if (!bar) { bar = create_bar(id, x, y, w, h); bars = g_list_append(bars, bar); }
        bar->seen = 1;
        gtk_widget_show(bar->window);
        for (i = 0; i < TAG_COUNT; i++) {
            set_class(bar->tags[i], "active", selected & (1U << i));
            set_class(bar->tags[i], "occupied", occupied & (1U << i));
            set_class(bar->tags[i], "urgent", urgent & (1U << i));
        }
    }
    XFree(value);
    for (p = bars; p; p = next) {
        Bar *bar = p->data;
        next = p->next;
        if (!bar->seen) { bars = g_list_delete_link(bars, p); gtk_widget_destroy(bar->window); g_free(bar); }
    }
    return G_SOURCE_CONTINUE;
}

static void update_labels(void) {
    GList *p;
    char clock[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(clock, sizeof clock, "󰥔 %a %d %b  %H:%M", &tm);
    for (p = bars; p; p = p->next) {
        Bar *bar = p->data;
        gtk_label_set_text(GTK_LABEL(bar->cpu), cpu_text);
        gtk_label_set_text(GTK_LABEL(bar->audio), audio_text);
        gtk_label_set_text(GTK_LABEL(bar->network), network_text);
        gtk_label_set_text(GTK_LABEL(bar->clock), clock);
        set_class(gtk_widget_get_parent(bar->audio), "muted", strstr(audio_text, "Muted") != NULL);
        set_class(gtk_widget_get_parent(bar->network), "offline", strstr(network_text, "Offline") != NULL);
    }
}

static char *command_output(const char *command) {
    char *out = NULL;
    int status = 0;
    if (!g_spawn_command_line_sync(command, &out, NULL, &status, NULL) || status != 0) {
        g_free(out); return NULL;
    }
    return out;
}

static gboolean apply_reading(gpointer data) {
    Reading *r = data;
    g_strlcpy(audio_text, r->audio, sizeof audio_text);
    g_strlcpy(network_text, r->network, sizeof network_text);
    g_free(r);
    g_atomic_int_set(&worker_running, 0);
    update_labels();
    return G_SOURCE_REMOVE;
}

static gpointer read_status(gpointer unused) {
    Reading *r = g_new0(Reading, 1);
    char *out, *percent;
    (void)unused;
    g_strlcpy(r->audio, "󰕾 Audio --", sizeof r->audio);
    out = command_output("pactl get-sink-mute @DEFAULT_SINK@");
    if (out && strstr(out, "yes")) g_strlcpy(r->audio, "󰝟 Muted", sizeof r->audio);
    else if (out && strstr(out, "no")) {
        char *volume = command_output("pactl get-sink-volume @DEFAULT_SINK@");
        if (volume && (percent = strchr(volume, '%'))) {
            char *start = percent;
            while (start > volume && g_ascii_isdigit(start[-1])) start--;
            if (start != percent) g_snprintf(r->audio, sizeof r->audio, "󰕾 %.*s%%", (int)(percent - start), start);
        }
        g_free(volume);
    }
    g_free(out);
    g_strlcpy(r->network, "󰤭 Offline", sizeof r->network);
    out = command_output("nmcli -t -f TYPE,STATE device status");
    if (out) {
        if (strstr(out, "wifi:connected")) g_strlcpy(r->network, "󰤨 Wi-Fi", sizeof r->network);
        else if (strstr(out, "ethernet:connected")) g_strlcpy(r->network, "󰈀 Ethernet", sizeof r->network);
        else if (strstr(out, ":connected")) g_strlcpy(r->network, "󰈀 Online", sizeof r->network);
    } else g_strlcpy(r->network, "󰤭 Network --", sizeof r->network);
    g_free(out);
    g_idle_add(apply_reading, r);
    return NULL;
}

static gboolean tick(gpointer unused) {
    FILE *f;
    unsigned long long u, n, s, idle, wait, irq, softirq, steal, total, idleall;
    static int seconds;
    (void)unused;
    f = fopen("/proc/stat", "r");
    if (f) {
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &wait, &irq, &softirq, &steal) == 8) {
            total = u + n + s + idle + wait + irq + softirq + steal; idleall = idle + wait;
            if (previous_total && total > (unsigned long long)previous_total && idleall >= (unsigned long long)previous_idle &&
                idleall - previous_idle <= total - previous_total)
                g_snprintf(cpu_text, sizeof cpu_text, " %llu%%", 100 * ((total - previous_total) - (idleall - previous_idle)) / (total - previous_total));
            previous_total = total; previous_idle = idleall;
        }
        fclose(f);
    }
    update_monitors(NULL);
    update_labels();
    if (seconds++ % 2 == 0 && g_atomic_int_compare_and_exchange(&worker_running, 0, 1))
        g_thread_unref(g_thread_new("bar-status", read_status, NULL));
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    GtkCssProvider *css;
    const char *config;
    char *path;
    GError *error = NULL;
    gtk_init(&argc, &argv);
    xdpy = XOpenDisplay(NULL);
    if (!xdpy) { fprintf(stderr, "dwm-bar: cannot open X display\n"); return 1; }
    root = DefaultRootWindow(xdpy);
    state_atom = XInternAtom(xdpy, "_DWM_BAR_STATE", False);
    view_atom = XInternAtom(xdpy, "_DWM_BAR_VIEW", False);
    css = gtk_css_provider_new();
    config = g_get_user_config_dir();
    path = g_build_filename(config, "dwm", "bar", "style.css", NULL);
    if (!gtk_css_provider_load_from_path(css, path, &error)) {
        fprintf(stderr, "dwm-bar: %s\n", error->message); g_clear_error(&error);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css); g_free(path);
    tick(NULL);
    g_timeout_add_seconds(1, tick, NULL);
    gtk_main();
    XCloseDisplay(xdpy);
    return 0;
}
