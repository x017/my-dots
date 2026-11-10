/* Application launcher built on CUI's command palette, coloured by the same
 * palette as the control center and the native Alt+Tab switcher. */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_icons.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../command_run.h"
#include "persian_font.h"
#include "theme.h"
#include "ui_fonts.h"
#include "launcher_exec.h"
#include "launcher_apps.h"

static const char *launcher_icon(const char *group) {
    if (!strcmp(group, "Development")) return CUI_ICON_CODE;
    if (!strcmp(group, "Education")) return CUI_ICON_NOTEBOOK_PEN;
    if (!strcmp(group, "Game")) return CUI_ICON_PLAY;
    if (!strcmp(group, "Graphics")) return CUI_ICON_IMAGE;
    if (!strcmp(group, "Network")) return CUI_ICON_GLOBE;
    if (!strcmp(group, "Office")) return CUI_ICON_FILE_TEXT;
    if (!strcmp(group, "Science")) return CUI_ICON_SPARKLES;
    if (!strcmp(group, "Settings")) return CUI_ICON_SETTINGS;
    if (!strcmp(group, "System")) return CUI_ICON_MONITOR;
    if (!strcmp(group, "Utility")) return CUI_ICON_BOX;
    if (!strcmp(group, "Video")) return CUI_ICON_VIDEO;
    if (!strcmp(group, "Audio") || !strcmp(group, "AudioVideo")) return CUI_ICON_MUSIC;
    return CUI_ICON_GRID_3X3;
}

static void launcher_spawn(const LauncherApp *app) {
    char argv[LAUNCHER_ARGV][LAUNCHER_ARG], *command[LAUNCHER_ARGV + 3];
    const char *terminal;
    pid_t middle;
    int count, total = 0, i;

    count = launcher_exec_argv(app->exec, app->name, app->icon, app->desktop, argv, LAUNCHER_ARGV);
    if (count <= 0) {
        fprintf(stderr, "dwm-launcher: cannot use the Exec line of %s\n", app->name);
        return;
    }
    if (app->terminal) {
        terminal = getenv("TERMINAL");
        command[total++] = (char *)(terminal && *terminal ? terminal : "ghostty");
        command[total++] = (char *)"-e";
    }
    for (i = 0; i < count && total < LAUNCHER_ARGV + 2; i++) command[total++] = argv[i];
    command[total] = NULL;
    middle = fork();
    if (middle == 0) {
        /* Detach twice: this launcher exits at once, so the application must
         * not stay its child. */
        if (fork() == 0) { setsid(); execvp(command[0], command); _exit(127); }
        _exit(0);
    }
    if (middle > 0) { int status; while (waitpid(middle, &status, 0) < 0 && errno == EINTR) ; }
}

#ifdef DWM_EMBEDDED_CUI
int launcher_cui_main(void) {
#else
int main(void) {
#endif
    cui_app *app;
    cui_ctx *ui;
    LauncherAppList apps;
    cui_command *commands = NULL;
    char summary[64];
    bool open = true;
    int i;

    signal(SIGPIPE, SIG_IGN);
    app = cui_app_create(&(cui_app_desc){ .title = "Dwm Launcher", .width = 660, .height = 520,
                                          .min_width = 480, .min_height = 360, .fixed_size = true,
                                          .theme = CUI_THEME_DARK });
    if (!app) { fprintf(stderr, "dwm-launcher: could not open an X11 window\n"); return 1; }
    ui = cui_app_ui(app);
    style(ui);
    /* The palette draws a 1 px edge and its separator lines from the BORDER
     * token. Here the palette is the whole window, so that line just reads as a
     * box around a box: match the token to the panel fill and let the panel's
     * own surface and shadow carry the shape. */
    {
        cui_theme *t = cui_theme_get(ui);
        t->colors[CUI_TOKEN_BORDER] = t->colors[CUI_TOKEN_SURFACE_RAISED];
    }
    ui_fonts_setup(ui);
    launcher_apps_read(&apps);
    if (apps.count) {
        commands = calloc((size_t)apps.count, sizeof *commands);
        if (!commands) { fprintf(stderr, "dwm-launcher: out of memory\n"); cui_app_destroy(app); return 1; }
    }
    for (i = 0; i < apps.count; i++) {
        commands[i].title = apps.items[i].name;
        commands[i].icon = launcher_icon(apps.items[i].group);
        commands[i].group = apps.items[i].group;
        commands[i].shortcut = apps.items[i].binary[0] ? apps.items[i].binary : NULL;
    }
    snprintf(summary, sizeof summary, "%d application%s found", apps.count, apps.count == 1 ? "" : "s");
    while (cui_app_frame(app)) {
        int chosen;
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 18, .gap = 4) {
            cui_text(ui, "APPLICATIONS", .size = 10, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, summary, .size = 10, .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
        }
        chosen = cui_command_palette(ui, &open, commands, apps.count);
        if (chosen >= 0 && chosen < apps.count) { launcher_spawn(&apps.items[chosen]); cui_app_quit(app); }
        else if (!open) cui_app_quit(app);
    }
    free(commands);
    cui_app_destroy(app);
    return 0;
}
