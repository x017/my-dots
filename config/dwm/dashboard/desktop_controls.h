/* Desktop controls run in the existing service worker, never the X event loop. */
#define DESKTOP_OUTPUTS 16
#define DESKTOP_MODES 192
#define ACT_DISPLAY_APPLY   ((Action)(ACT_AUDIO_VOLUME + 1))
#define ACT_DISPLAY_ARRANGE ((Action)(ACT_AUDIO_VOLUME + 2))
#define ACT_DISPLAY_POWER   ((Action)(ACT_AUDIO_VOLUME + 3))
#define ACT_DISPLAY_PRIMARY ((Action)(ACT_AUDIO_VOLUME + 4))
#define ACT_DISPLAY_ROTATE  ((Action)(ACT_AUDIO_VOLUME + 5))
#define ACT_DISPLAY_SCALE   ((Action)(ACT_AUDIO_VOLUME + 6))
typedef struct { char output[128], mode[64], rate[32]; int current; } DisplayMode;
/* Geometry is what xrandr reports for a connected output; an output with no
 * WxH+X+Y is connected but switched off. */
typedef struct { char name[128], rotation[16]; int x, y, w, h, primary, active; } DisplayOutput;
typedef struct {
    DisplayOutput outputs[DESKTOP_OUTPUTS]; int output_count;
    DisplayMode modes[DESKTOP_MODES]; int mode_count, xrandr_available;
} DesktopState;

static void desktop_read(DesktopState *d) {
    char output[MAX_OUTPUT], name[128], *line, *save, *token, *more;
    char *argv[] = {"xrandr", "--query", NULL};
    DisplayOutput *cur = NULL;
    memset(d, 0, sizeof *d);
    if (!run_command(argv, NULL, output, sizeof output, 4000)) {
        d->xrandr_available = 1;
        for (line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (*line != ' ' && *line != '\t') {
                char state[32];
                cur = NULL;
                if (sscanf(line, "%127s %31s", name, state) != 2 || strcmp(state, "connected") ||
                    d->output_count >= DESKTOP_OUTPUTS) continue;
                cur = &d->outputs[d->output_count++];
                copystr(cur->name, sizeof cur->name, name);
                copystr(cur->rotation, sizeof cur->rotation, "normal");
                /* After the state xrandr prints "primary", the WxH+X+Y frame and
                 * the rotation, then "(normal left inverted right ...)". Stop at
                 * the parenthesis so that list is not read as the rotation. */
                strtok_r(line, " \t\r", &more);
                strtok_r(NULL, " \t\r", &more);
                while ((token = strtok_r(NULL, " \t\r", &more))) {
                    int w = 0, h = 0, x = 0, y = 0;
                    if (token[0] == '(') break;
                    if (!strcmp(token, "primary")) { cur->primary = 1; continue; }
                    if (sscanf(token, "%dx%d+%d+%d", &w, &h, &x, &y) == 4 && w > 0 && h > 0) {
                        cur->active = 1; cur->w = w; cur->h = h; cur->x = x; cur->y = y; continue;
                    }
                    if (!strcmp(token, "normal") || !strcmp(token, "left") ||
                        !strcmp(token, "right") || !strcmp(token, "inverted"))
                        copystr(cur->rotation, sizeof cur->rotation, token);
                }
                continue;
            }
            if (!cur || !(token = strtok_r(line, " \t\r", &more))) continue;
            { unsigned int w, h; char extra; if (sscanf(token, "%ux%u%c", &w, &h, &extra) != 2 || !w || !h) continue; }
            if (strlen(token) >= sizeof d->modes[0].mode) continue;
            copystr(name, sizeof name, token);
            while ((token = strtok_r(NULL, " \t\r", &more)) && d->mode_count < DESKTOP_MODES) {
                char *end; double rate = strtod(token, &end); int active;
                if (end == token || rate <= 0 || strspn(end, "*+") != strlen(end)) continue;
                active = strchr(end, '*') != NULL; *end = 0;
                DisplayMode *m = &d->modes[d->mode_count++];
                copystr(m->output, sizeof m->output, cur->name); memcpy(m->mode, name, strlen(name) + 1);
                copystr(m->rate, sizeof m->rate, token); m->current = active;
            }
        }
    }

}

/* xrandr arguments only ever come from this dashboard, but they are confined
 * to a whitelist anyway so a stray value cannot become an extra flag. */
static int display_token(const char *value) {
    size_t n = strcspn(value, " \t\r\n\"'`$\\;|&<>");
    return value[0] && value[0] != '-' && n == strlen(value) && n < 128;
}
static int display_index(const char *value, const char *const *allowed) {
    int i;
    for (i = 0; allowed[i]; i++) if (!strcmp(value, allowed[i])) return i;
    return -1;
}

/* Match dwm's existing per-output restore file. */
static int desktop_save_mode(const char *output, const char *mode, const char *rate) {
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX], name[256], *line = NULL; size_t len = 0;
    FILE *in, *out; int fd, ok = 1, newline = 0;
    if (base && *base == '/') { if (snprintf(dir, sizeof dir, "%s", base) >= (int)sizeof dir) return 0; }
    else if (home && *home) { if (snprintf(dir, sizeof dir, "%s/.config", home) >= (int)sizeof dir) return 0; }
    else return 0;
    if (mkdir(dir, 0700) && errno != EEXIST) return 0;
    if (snprintf(path, sizeof path, "%s/refreshrate.conf", dir) >= (int)sizeof path ||
        snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path) >= (int)sizeof tmp) return 0;
    in = fopen(path, "r"); if (!in && errno != ENOENT) return 0;
    fd = mkstemp(tmp); if (fd < 0) { if (in) fclose(in); return 0; }
    out = fdopen(fd, "w"); if (!out) { close(fd); unlink(tmp); if (in) fclose(in); return 0; }
    if (in) {
        while (getline(&line, &len, in) >= 0) {
            if (sscanf(line, "%255s", name) == 1 && !strcmp(name, output)) continue;
            if (fputs(line, out) == EOF) { ok = 0; break; }
            newline = line[0] && line[strlen(line)-1] != '\n';
        }
        if (ferror(in)) ok = 0;
        if (fclose(in)) ok = 0;
    }
    free(line); if (newline && fputc('\n', out) == EOF) ok = 0;
    if (fprintf(out, "%s %s %s\n", output, mode, rate) < 0) ok = 0;
    if (fclose(out)) ok = 0;
    if (ok && !rename(tmp, path)) return 1;
    unlink(tmp); return 0;
}

static int desktop_execute(Request *r, char *message, size_t cap) {
    char out[MAX_OUTPUT], mode[64], rate[32], second[128], relation[32], extra;
    if (!display_token(r->arg)) return -1;
    if (r->type == ACT_DISPLAY_APPLY) {
        char *argv[] = {"xrandr", "--output", r->arg, "--mode", mode, "--rate", rate, NULL};
        if (strpbrk(r->arg, " \t\r\n") || sscanf(r->password, "%63s %31s %c", mode, rate, &extra) != 2) return -1;
        if (run_command(argv, NULL, out, sizeof out, 10000)) { copystr(message, cap, "xrandr rejected that resolution."); return -1; }
        if (!desktop_save_mode(r->arg, mode, rate)) { copystr(message, cap, "Display applied, but could not save for restart."); return 1; }
        copystr(message, cap, "Display mode applied and saved."); return 0;
    }
    if (r->type == ACT_DISPLAY_ARRANGE) {
        static const char *const relations[] = {"left-of", "right-of", "above", "below", "same-as", NULL};
        static const char *const flags[] = {"--left-of", "--right-of", "--above", "--below", "--same-as"};
        int which;
        char *argv[] = {"xrandr", "--output", r->arg, NULL, second, NULL};
        if (sscanf(r->password, "%31s %127s %c", relation, second, &extra) != 2) return -1;
        which = display_index(relation, relations);
        if (which < 0 || !display_token(second)) return -1;
        argv[3] = (char *)flags[which];
        if (run_command(argv, NULL, out, sizeof out, 10000)) { copystr(message, cap, "xrandr rejected that arrangement."); return -1; }
        copystr(message, cap, which == 4 ? "Monitors now mirror each other." : "Monitors extended.");
        return 0;
    }
    if (r->type == ACT_DISPLAY_POWER) {
        char *argv[] = {"xrandr", "--output", r->arg, r->value ? "--auto" : "--off", NULL};
        if (run_command(argv, NULL, out, sizeof out, 10000)) { copystr(message, cap, "xrandr could not change that monitor."); return -1; }
        copystr(message, cap, r->value ? "Monitor enabled." : "Monitor turned off."); return 0;
    }
    if (r->type == ACT_DISPLAY_PRIMARY) {
        char *argv[] = {"xrandr", "--output", r->arg, "--primary", NULL};
        if (run_command(argv, NULL, out, sizeof out, 10000)) { copystr(message, cap, "xrandr could not set the primary monitor."); return -1; }
        copystr(message, cap, "Primary monitor changed."); return 0;
    }
    if (r->type == ACT_DISPLAY_ROTATE) {
        static const char *const rotations[] = {"normal", "left", "right", "inverted", NULL};
        char *argv[] = {"xrandr", "--output", r->arg, "--rotate", r->password, NULL};
        if (display_index(r->password, rotations) < 0) return -1;
        if (run_command(argv, NULL, out, sizeof out, 10000)) { copystr(message, cap, "xrandr rejected that rotation."); return -1; }
        copystr(message, cap, "Rotation applied."); return 0;
    }
    if (r->type == ACT_DISPLAY_SCALE) {
        double x, y; char separator;
        char *argv[] = {"xrandr", "--output", r->arg, "--scale", r->password, NULL};
        if (sscanf(r->password, "%lfx%lf%c", &x, &y, &separator) != 2 || x <= 0 || y <= 0) return -1;
        if (run_command(argv, NULL, out, sizeof out, 10000)) { copystr(message, cap, "xrandr rejected that scale."); return -1; }
        copystr(message, cap, "Scale applied."); return 0;
    }
    return -1;
}
