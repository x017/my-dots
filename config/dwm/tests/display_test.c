/* Runs the real desktop_read() parser and desktop_execute() command builder
 * against a stand-in xrandr, so both the parsed geometry and the exact xrandr
 * command lines are checked. */
#define _DEFAULT_SOURCE
#define DASHBOARD_NO_SNAPSHOT
#define DASHBOARD_NO_ACTIONS
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../dashboard_backend.h"
#include "../dashboard/desktop_controls.h"

static char workdir[64], logpath[96], querypath[96];

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) != EOF && !fclose(f));
}

static void run_case(Action type, const char *output, const char *params, int value,
                     int expected, const char *command) {
    Request r; char message[256] = ""; char got[512] = "";
    FILE *f;
    memset(&r, 0, sizeof r);
    r.type = type; r.value = value;
    snprintf(r.arg, sizeof r.arg, "%s", output);
    snprintf(r.password, sizeof r.password, "%s", params);
    unlink(logpath);
    assert(desktop_execute(&r, message, sizeof message) == expected);
    f = fopen(logpath, "r");
    if (f) { if (!fgets(got, sizeof got, f)) got[0] = 0; fclose(f); }
    got[strcspn(got, "\n")] = 0;
    if (command) assert(!strcmp(got, command));
    else assert(!got[0]);
}

int main(void) {
    DesktopState d; char path[PATH_MAX], line[PATH_MAX], saved[96];
    const DisplayOutput *hdmi = NULL, *dp = NULL, *vga = NULL;
    int i, hdmi_modes = 0, current_found = 0;
    FILE *f;

    strcpy(workdir, "/tmp/dwm-display-XXXXXX");
    assert(mkdtemp(workdir));
    snprintf(logpath, sizeof logpath, "%s/argv.log", workdir);
    snprintf(querypath, sizeof querypath, "%s/query.txt", workdir);
    snprintf(path, sizeof path, "%s/xrandr", workdir);
    write_file(querypath,
        "Screen 0: minimum 320 x 200, current 4480 x 1440, maximum 16384 x 16384\n"
        "DP-1 connected 1920x1080+2560+0 left (normal left inverted right x axis y axis) 476mm x 268mm\n"
        "   1920x1080     60.00*+  59.94    50.00\n"
        "   1680x1050     59.88\n"
        "HDMI-1 connected primary 2560x1440+0+0 (normal left inverted right x axis y axis) 597mm x 336mm\n"
        "   2560x1440     59.95*+ 143.97\n"
        "   1920x1080    120.00    60.00\n"
        "VGA-1 connected (normal left inverted right x axis y axis) 0mm x 0mm\n"
        "eDP-1 disconnected (normal left inverted right x axis y axis)\n");
    write_file(path, "#!/bin/sh\n"
                     "if [ \"$1\" = \"--query\" ]; then cat \"$XRANDR_QUERY\"; exit 0; fi\n"
                     "echo \"$@\" >> \"$XRANDR_LOG\"\n"
                     "exit 0\n");
    assert(chmod(path, 0755) == 0);
    snprintf(line, sizeof line, "%s:/usr/bin:/bin", workdir);
    snprintf(saved, sizeof saved, "%s/config", workdir);
    assert(mkdir(saved, 0700) == 0);
    setenv("PATH", line, 1); setenv("XRANDR_QUERY", querypath, 1);
    setenv("XRANDR_LOG", logpath, 1); setenv("XDG_CONFIG_HOME", saved, 1);

    /* Parsing: connected outputs, geometry, primary flag, rotation, modes. */
    desktop_read(&d);
    assert(d.xrandr_available && d.output_count == 3);
    for (i = 0; i < d.output_count; i++) {
        if (!strcmp(d.outputs[i].name, "HDMI-1")) hdmi = &d.outputs[i];
        if (!strcmp(d.outputs[i].name, "DP-1")) dp = &d.outputs[i];
        if (!strcmp(d.outputs[i].name, "VGA-1")) vga = &d.outputs[i];
    }
    assert(hdmi && dp && vga);
    assert(hdmi->primary && hdmi->active && hdmi->w == 2560 && hdmi->h == 1440 &&
           hdmi->x == 0 && hdmi->y == 0 && !strcmp(hdmi->rotation, "normal"));
    assert(!dp->primary && dp->active && dp->w == 1920 && dp->h == 1080 &&
           dp->x == 2560 && dp->y == 0 && !strcmp(dp->rotation, "left"));
    assert(!vga->active && !vga->primary && !strcmp(vga->rotation, "normal"));
    for (i = 0; i < d.mode_count; i++) {
        if (strcmp(d.modes[i].output, "HDMI-1")) continue;
        hdmi_modes++;
        if (!strcmp(d.modes[i].mode, "2560x1440") && !strcmp(d.modes[i].rate, "59.95"))
            { assert(d.modes[i].current); current_found = 1; }
        else assert(!d.modes[i].current);
    }
    assert(d.mode_count == 8 && hdmi_modes == 4 && current_found);

    /* Command lines actually handed to xrandr. */
    run_case(ACT_DISPLAY_APPLY, "HDMI-1", "2560x1440 143.97", 0, 0,
             "--output HDMI-1 --mode 2560x1440 --rate 143.97");
    snprintf(path, sizeof path, "%s/refreshrate.conf", saved);
    f = fopen(path, "r");
    assert(f && fgets(line, sizeof line, f) && !strcmp(line, "HDMI-1 2560x1440 143.97\n"));
    fclose(f);
    run_case(ACT_DISPLAY_ARRANGE, "HDMI-1", "right-of DP-1", 0, 0, "--output HDMI-1 --right-of DP-1");
    run_case(ACT_DISPLAY_ARRANGE, "HDMI-1", "left-of DP-1", 0, 0, "--output HDMI-1 --left-of DP-1");
    run_case(ACT_DISPLAY_ARRANGE, "DP-1", "above HDMI-1", 0, 0, "--output DP-1 --above HDMI-1");
    run_case(ACT_DISPLAY_ARRANGE, "DP-1", "below HDMI-1", 0, 0, "--output DP-1 --below HDMI-1");
    run_case(ACT_DISPLAY_ARRANGE, "HDMI-1", "same-as DP-1", 0, 0, "--output HDMI-1 --same-as DP-1");
    run_case(ACT_DISPLAY_POWER, "HDMI-1", "", 0, 0, "--output HDMI-1 --off");
    run_case(ACT_DISPLAY_POWER, "VGA-1", "", 1, 0, "--output VGA-1 --auto");
    run_case(ACT_DISPLAY_PRIMARY, "DP-1", "", 0, 0, "--output DP-1 --primary");
    run_case(ACT_DISPLAY_ROTATE, "DP-1", "inverted", 0, 0, "--output DP-1 --rotate inverted");
    run_case(ACT_DISPLAY_SCALE, "HDMI-1", "0.8x0.8", 0, 0, "--output HDMI-1 --scale 0.8x0.8");

    /* Rejected before xrandr is ever started. */
    run_case(ACT_DISPLAY_ARRANGE, "HDMI-1", "--mode DP-1", 0, -1, NULL);
    run_case(ACT_DISPLAY_ARRANGE, "HDMI-1", "right-of", 0, -1, NULL);
    run_case(ACT_DISPLAY_ARRANGE, "HDMI-1", "right-of DP 1", 0, -1, NULL);
    run_case(ACT_DISPLAY_ROTATE, "DP-1", "sideways", 0, -1, NULL);
    run_case(ACT_DISPLAY_SCALE, "HDMI-1", "big", 0, -1, NULL);
    run_case(ACT_DISPLAY_SCALE, "HDMI-1", "0x0.8", 0, -1, NULL);
    run_case(ACT_DISPLAY_APPLY, "HDMI 1", "2560x1440 143.97", 0, -1, NULL);
    run_case(ACT_DISPLAY_PRIMARY, "--primary", "", 0, -1, NULL);
    run_case(ACT_DISPLAY_POWER, "HDMI-1;reboot", "", 1, -1, NULL);
    run_case(ACT_NONE, "HDMI-1", "", 0, -1, NULL);

    snprintf(line, sizeof line, "rm -rf %s", workdir);
    assert(system(line) == 0);
    puts("xrandr parsing, arrangement commands and input validation passed");
    return 0;
}
