/* A small GTK3 wallpaper picker for dwm. Images are applied with feh. */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>

typedef struct {
    GtkWidget *window, *flow, *search, *preview, *name, *hint, *apply;
    char *directory, *selected, *query;
    GThreadPool *loader;
    GFileMonitor *monitor;
    guint reload_id, preview_generation;
    gboolean applying;
} App;

typedef struct {
    GtkWidget *image;
    char *path;
    int width, height;
    guint generation;
    gboolean preview;
    GdkPixbuf *pixbuf;
} ImageJob;

static App app;
static void reload_images(void);
static void folder_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                           GFileMonitorEvent event, gpointer unused);

static void watch_folder(void) {
    GFile *folder;
    if (app.monitor) return;
    folder = g_file_new_for_path(app.directory);
    app.monitor = g_file_monitor_directory(folder, G_FILE_MONITOR_NONE, NULL, NULL);
    if (app.monitor) g_signal_connect(app.monitor, "changed", G_CALLBACK(folder_changed), NULL);
    g_object_unref(folder);
}

/* Decoding takes place off the GTK thread. Keep the image alive if a folder
 * refresh destroys its card before the decoder returns. */
static gboolean finish_image(gpointer data) {
    ImageJob *job = data;
    if (!job->preview || job->generation == app.preview_generation) {
        if (job->pixbuf) gtk_image_set_from_pixbuf(GTK_IMAGE(job->image), job->pixbuf);
        else gtk_image_set_from_icon_name(GTK_IMAGE(job->image), "image-missing-symbolic", GTK_ICON_SIZE_DIALOG);
    }
    if (job->pixbuf) g_object_unref(job->pixbuf);
    g_object_unref(job->image);
    g_free(job->path);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static void load_image(gpointer data, gpointer unused) {
    ImageJob *job = data;
    (void)unused;
    job->pixbuf = gdk_pixbuf_new_from_file_at_scale(job->path, job->width, job->height, TRUE, NULL);
    g_idle_add(finish_image, job);
}

static void queue_image(GtkWidget *image, const char *path, int width, int height, gboolean preview) {
    ImageJob *job = g_new0(ImageJob, 1);
    job->image = g_object_ref(image);
    job->path = g_strdup(path);
    job->width = width; job->height = height;
    job->preview = preview; job->generation = app.preview_generation;
    g_thread_pool_push(app.loader, job, NULL);
}

static void set_hint(const char *message) {
    gtk_label_set_text(GTK_LABEL(app.hint), message);
}

static void selection_changed(GtkFlowBox *box, gpointer unused) {
    GList *selected = gtk_flow_box_get_selected_children(box);
    const char *path = selected ? g_object_get_data(G_OBJECT(selected->data), "path") : NULL;
    char *base;
    (void)unused;
    g_free(app.selected);
    app.selected = g_strdup(path);
    app.preview_generation++;
    gtk_widget_set_sensitive(app.apply, app.selected && !app.applying);
    gtk_image_clear(GTK_IMAGE(app.preview));
    if (app.selected) {
        base = g_filename_display_basename(app.selected);
        gtk_label_set_text(GTK_LABEL(app.name), base);
        g_free(base);
        queue_image(app.preview, app.selected, 420, 320, TRUE);
        set_hint("Ready to apply on your displays");
    } else {
        gtk_label_set_text(GTK_LABEL(app.name), "Select a wallpaper");
    }
    g_list_free(selected);
}

static void apply_finished(GObject *object, GAsyncResult *result, gpointer unused) {
    GError *error = NULL;
    (void)unused;
    app.applying = FALSE;
    gtk_widget_set_sensitive(app.apply, app.selected != NULL);
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(object), result, &error)) {
        char *message = g_strdup_printf("Could not apply wallpaper: %s", error->message);
        set_hint(message); g_free(message); g_error_free(error);
    } else set_hint("Wallpaper applied · feh saved ~/.fehbg for restoring at login");
}

static void apply_wallpaper(GtkWidget *widget, gpointer unused) {
    GError *error = NULL;
    GSubprocess *process;
    const char *args[5];
    (void)widget; (void)unused;
    if (!app.selected || app.applying) return;
    args[0] = "feh"; args[1] = "--bg-fill"; args[2] = "--"; args[3] = app.selected; args[4] = NULL;
    process = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (!process) {
        char *message = g_strdup_printf("Could not start feh: %s", error->message);
        set_hint(message); g_free(message); g_error_free(error);
        return;
    }
    app.applying = TRUE;
    gtk_widget_set_sensitive(app.apply, FALSE);
    set_hint("Applying wallpaper…");
    g_subprocess_wait_check_async(process, NULL, apply_finished, NULL);
    g_object_unref(process);
}

static void child_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer unused) {
    (void)unused;
    gtk_flow_box_select_child(box, child);
    apply_wallpaper(NULL, NULL);
}

static gboolean filter_card(GtkFlowBoxChild *child, gpointer unused) {
    const char *name = g_object_get_data(G_OBJECT(child), "search-name");
    (void)unused;
    return !app.query || !*app.query || (name && strstr(name, app.query));
}

static void search_changed(GtkSearchEntry *entry, gpointer unused) {
    (void)unused;
    g_free(app.query);
    app.query = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(entry)), -1);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(app.flow));
}

static gboolean supported_image(const char *name) {
    char *lower = g_ascii_strdown(name, -1);
    gboolean supported = g_str_has_suffix(lower, ".jpg") || g_str_has_suffix(lower, ".jpeg") ||
        g_str_has_suffix(lower, ".png") || g_str_has_suffix(lower, ".webp") ||
        g_str_has_suffix(lower, ".bmp") || g_str_has_suffix(lower, ".gif");
    g_free(lower);
    return supported;
}

static gint compare_names(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

static void reload_images(void) {
    GError *error = NULL;
    GDir *dir;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GList *children, *p;
    const char *entry;
    char *previous = g_strdup(app.selected);
    guint i;
    GtkFlowBoxChild *reselect = NULL;
    children = gtk_container_get_children(GTK_CONTAINER(app.flow));
    for (p = children; p; p = p->next) gtk_widget_destroy(GTK_WIDGET(p->data));
    g_list_free(children);
    g_clear_pointer(&app.selected, g_free);
    app.preview_generation++;
    gtk_image_clear(GTK_IMAGE(app.preview));
    gtk_label_set_text(GTK_LABEL(app.name), "Select a wallpaper");
    gtk_widget_set_sensitive(app.apply, FALSE);
    dir = g_dir_open(app.directory, 0, &error);
    if (!dir) {
        char *message = g_strdup_printf("Can't open %s: %s", app.directory, error->message);
        set_hint(message); g_free(message); g_error_free(error);
        g_ptr_array_free(paths, TRUE); g_free(previous); return;
    }
    watch_folder();
    while ((entry = g_dir_read_name(dir))) {
        char *path;
        if (!supported_image(entry)) continue;
        path = g_build_filename(app.directory, entry, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) g_ptr_array_add(paths, path);
        else g_free(path);
    }
    g_dir_close(dir);
    g_ptr_array_sort(paths, compare_names);
    for (i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        char *base = g_filename_display_basename(path);
        char *folded = g_utf8_casefold(base, -1);
        GtkWidget *child = gtk_flow_box_child_new(), *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *image = gtk_image_new_from_icon_name("image-x-generic-symbolic", GTK_ICON_SIZE_DIALOG);
        GtkWidget *label = gtk_label_new(base);
        gtk_widget_set_size_request(image, 180, 112);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
        gtk_widget_set_halign(label, GTK_ALIGN_FILL);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "wallpaper-card");
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "card-label");
        gtk_container_add(GTK_CONTAINER(child), card);
        gtk_box_pack_start(GTK_BOX(card), image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), label, FALSE, FALSE, 0);
        g_object_set_data_full(G_OBJECT(child), "path", g_strdup(path), g_free);
        g_object_set_data_full(G_OBJECT(child), "search-name", folded, g_free);
        gtk_flow_box_insert(GTK_FLOW_BOX(app.flow), child, -1);
        if (previous && !strcmp(previous, path)) reselect = GTK_FLOW_BOX_CHILD(child);
        queue_image(image, path, 180, 112, FALSE);
        g_free(base);
    }
    gtk_widget_show_all(app.flow);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(app.flow));
    if (reselect) gtk_flow_box_select_child(GTK_FLOW_BOX(app.flow), reselect);
    else if (paths->len) set_hint("Select an image, then apply · double-click to apply immediately");
    else set_hint("No images found. Add wallpapers to the folder and press Refresh.");
    g_ptr_array_free(paths, TRUE); g_free(previous);
}

static gboolean reload_timeout(gpointer unused) {
    (void)unused; app.reload_id = 0; reload_images(); return G_SOURCE_REMOVE;
}
static void folder_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer unused) {
    (void)monitor; (void)file; (void)other; (void)event; (void)unused;
    if (app.reload_id) g_source_remove(app.reload_id);
    app.reload_id = g_timeout_add(500, reload_timeout, NULL);
}
static void refresh_clicked(GtkWidget *button, gpointer unused) { (void)button; (void)unused; reload_images(); }
static gboolean key_press(GtkWidget *widget, GdkEventKey *event, gpointer unused) {
    (void)widget; (void)unused;
    if (event->keyval == GDK_KEY_Escape) { gtk_main_quit(); return TRUE; }
    return FALSE;
}
static void close_window(GtkWidget *widget, gpointer unused) { (void)widget; (void)unused; gtk_main_quit(); }

static const char css[] =
    "* { font-family: monospace; }"
    "window, scrolledwindow, viewport, flowbox { background: #090b0d; color: #adbac7; }"
    ".title { color: #adbac7; font-size: 15px; font-weight: bold; letter-spacing: 2px; }"
    ".subtitle, .hint { color: #8a949e; font-size: 11px; }"
    ".accent { color: #7fa8c9; font-size: 11px; }"
    ".divider { background: #1e2025; min-height: 1px; }"
    "entry { background: #121417; border: 1px solid #2d3035; border-radius: 6px; color: #adbac7; padding: 7px; }"
    "button { background-image: none; background: #121417; border: 1px solid #2d3035; border-radius: 6px; color: #adbac7; padding: 7px 12px; box-shadow: none; text-shadow: none; }"
    "button:hover { background: #181a1e; border-color: #7fa8c9; }"
    "button.apply { background: #f2d49e; border-color: #f2d49e; color: #090b0d; font-weight: bold; }"
    "button.apply:disabled { background: #181a1e; border-color: #1e2025; color: #64778d; }"
    "flowboxchild { padding: 3px; border-radius: 8px; }"
    "flowboxchild:selected { background: #7fa8c9; }"
    ".wallpaper-card, .preview-panel { background: #121417; border: 1px solid #1e2025; border-radius: 6px; padding: 7px; }"
    ".card-label { color: #adbac7; padding: 7px 2px 2px; font-size: 11px; }"
    ".preview-name { color: #f2d49e; font-size: 13px; font-weight: bold; }";

int main(int argc, char **argv) {
    GtkCssProvider *provider;
    GtkWidget *outer, *header, *heading, *subtitle, *divider, *content, *scroll, *side, *preview_title, *footer, *refresh, *close;
    const char *env;
    g_set_prgname("dwm-wallpapers");
    gtk_init(&argc, &argv);
    if (argc > 2) { g_printerr("Usage: dwm-wallpapers [DIRECTORY]\n"); return 2; }
    env = g_getenv("DWM_WALLPAPERS_DIR");
    app.directory = argc == 2 ? g_canonicalize_filename(argv[1], NULL) :
        env && *env ? g_canonicalize_filename(env, NULL) : g_build_filename(g_get_home_dir(), "Downloads", "wallpaper", NULL);
    app.loader = g_thread_pool_new(load_image, NULL, 4, FALSE, NULL);
    if (!app.loader) { g_printerr("Failed to create image loader\n"); return 1; }
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Wallpapers");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 980, 620);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    g_signal_connect(app.window, "destroy", G_CALLBACK(close_window), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(key_press), NULL);
    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 18);
    gtk_container_add(GTK_CONTAINER(app.window), outer);
    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    subtitle = gtk_label_new("W A L L P A P E R S");
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "title");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(heading), subtitle, FALSE, FALSE, 0);
    subtitle = gtk_label_new(app.directory);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "subtitle");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(heading), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), heading, TRUE, TRUE, 0);
    refresh = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(header), refresh, FALSE, FALSE, 0);
    close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(close_window), NULL);
    gtk_box_pack_end(GTK_BOX(header), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);
    divider = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(divider), "divider");
    gtk_box_pack_start(GTK_BOX(outer), divider, FALSE, FALSE, 0);
    content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_pack_start(GTK_BOX(outer), content, TRUE, TRUE, 0);
    heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(content), heading, TRUE, TRUE, 0);
    app.search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.search), "Search wallpapers…");
    g_signal_connect(app.search, "search-changed", G_CALLBACK(search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(heading), app.search, FALSE, FALSE, 0);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(heading), scroll, TRUE, TRUE, 0);
    app.flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(app.flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(app.flow), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(app.flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(app.flow), 3);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(app.flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(app.flow), 8);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(app.flow), filter_card, NULL, NULL);
    g_signal_connect(app.flow, "selected-children-changed", G_CALLBACK(selection_changed), NULL);
    g_signal_connect(app.flow, "child-activated", G_CALLBACK(child_activated), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), app.flow);
    side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(side, 310, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(side), "preview-panel");
    gtk_box_pack_start(GTK_BOX(content), side, FALSE, FALSE, 0);
    preview_title = gtk_label_new("P R E V I E W");
    gtk_style_context_add_class(gtk_widget_get_style_context(preview_title), "accent");
    gtk_widget_set_halign(preview_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(side), preview_title, FALSE, FALSE, 0);
    app.preview = gtk_image_new();
    gtk_widget_set_size_request(app.preview, 300, 220);
    gtk_box_pack_start(GTK_BOX(side), app.preview, TRUE, TRUE, 0);
    app.name = gtk_label_new("Select a wallpaper");
    gtk_label_set_ellipsize(GTK_LABEL(app.name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.name), "preview-name");
    gtk_box_pack_start(GTK_BOX(side), app.name, FALSE, FALSE, 0);
    app.apply = gtk_button_new_with_label("Set wallpaper");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.apply), "apply");
    gtk_widget_set_sensitive(app.apply, FALSE);
    g_signal_connect(app.apply, "clicked", G_CALLBACK(apply_wallpaper), NULL);
    gtk_box_pack_start(GTK_BOX(side), app.apply, FALSE, FALSE, 0);
    footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.hint = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app.hint), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(app.hint, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.hint), "hint");
    gtk_box_pack_start(GTK_BOX(footer), app.hint, TRUE, TRUE, 0);
    subtitle = gtk_label_new("ESC  CLOSE");
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "accent");
    gtk_box_pack_end(GTK_BOX(footer), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), footer, FALSE, FALSE, 0);
    gtk_widget_show_all(app.window);
    reload_images();
    gtk_main();
    return 0;
}
