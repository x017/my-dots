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
#include "persian_font.h"

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
            message[0] = 0; dashboard_action_error[0] = 0;
            if (request.type == ACT_DISPLAY_APPLY)
                rc = desktop_execute(&request, message, sizeof message);
            else rc = execute(&request);
            if (rc == 0 && !message[0]) copystr(message, sizeof message, "Done.");
            else if (rc && dashboard_action_error[0]) copystr(message, sizeof message, dashboard_action_error);
            else if (rc && request.type == ACT_WIFI_CONNECT) copystr(message, sizeof message, "Connection failed. Check password, permissions or NetworkManager.");
            else if (rc && request.type == ACT_BT_PAIR) copystr(message, sizeof message, "Pairing failed. You may need a Bluetooth pairing agent; open Bluetooth settings.");
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

#include "pages.h"

#ifdef DWM_EMBEDDED_CUI
int dashboard_cui_main(void) {
#else
int main(void) {
#endif
    cui_app *app;
    cui_ctx *ui;
    UIState state = { .page = 4 };
    signal(SIGPIPE, SIG_IGN);
    app = cui_app_create(&(cui_app_desc){ .title = "Dwm Control Center", .width = 760, .height = 700,
                                         .min_width = 480, .min_height = 400, .theme = CUI_THEME_DARK });
    if (!app) { fprintf(stderr, "dwm-dashboard: could not open an X11 window\n"); return 1; }
    ui = cui_app_ui(app);
    style(ui); load_monospace(ui); cui_persian_fallback(ui);
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
        if (dashboard_shell(ui, &state, &snapshot, &desktop, busy)) cui_app_quit(app);
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
