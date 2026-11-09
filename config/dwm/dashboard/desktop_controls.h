/* Desktop controls run in the existing service worker, never the X event loop. */
#define DESKTOP_OUTPUTS 16
#define DESKTOP_MODES 192
#define DESKTOP_WALLPAPERS 100
#define ACT_DISPLAY_APPLY ((Action)(ACT_AUDIO_VOLUME + 1))
#define ACT_WALLPAPER_APPLY ((Action)(ACT_AUDIO_VOLUME + 2))
typedef struct { char output[128], mode[64], rate[32]; int current; } DisplayMode;
typedef struct {
    char outputs[DESKTOP_OUTPUTS][128]; int output_count;
    DisplayMode modes[DESKTOP_MODES]; int mode_count, xrandr_available;
    char wallpaper_names[DESKTOP_WALLPAPERS][256]; int wallpaper_count, wallpaper_available;
} DesktopState;

static int wallpaper_dir(char *path, size_t cap) {
    const char *dir = getenv("DWM_WALLPAPERS_DIR"), *home = getenv("HOME"); int n;
    if (dir && *dir) n = snprintf(path, cap, "%s", dir);
    else if (home && *home) n = snprintf(path, cap, "%s/Downloads/wallpaper", home);
    else n = snprintf(path, cap, ".");
    return n >= 0 && (size_t)n < cap;
}
static int wallpaper_ext(const char *name) {
    const char *p = strrchr(name, '.');
    return p && (!strcasecmp(p, ".jpg") || !strcasecmp(p, ".jpeg") || !strcasecmp(p, ".png") ||
                 !strcasecmp(p, ".webp") || !strcasecmp(p, ".bmp") || !strcasecmp(p, ".gif"));
}
static int wallpaper_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void desktop_read(DesktopState *d) {
    char output[MAX_OUTPUT], dir[PATH_MAX], current[128] = "", *line, *save;
    char *argv[] = {"xrandr", "--query", NULL}; DIR *dp; struct dirent *entry;
    memset(d, 0, sizeof *d);
    if (!run_command(argv, NULL, output, sizeof output, 4000)) {
        d->xrandr_available = 1;
        for (line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char name[128], state[32], *token, *more;
            if (*line != ' ' && *line != '\t') {
                current[0] = 0;
                if (sscanf(line, "%127s %31s", name, state) == 2 && !strcmp(state, "connected") && d->output_count < DESKTOP_OUTPUTS) {
                    copystr(current, sizeof current, name);
                    copystr(d->outputs[d->output_count++], sizeof d->outputs[0], name);
                }
                continue;
            }
            if (!current[0] || !(token = strtok_r(line, " \t\r", &more))) continue;
            { unsigned int w, h; char extra; if (sscanf(token, "%ux%u%c", &w, &h, &extra) != 2 || !w || !h) continue; }
            if (strlen(token) >= sizeof d->modes[0].mode) continue;
            copystr(name, sizeof name, token);
            while ((token = strtok_r(NULL, " \t\r", &more)) && d->mode_count < DESKTOP_MODES) {
                char *end; double rate = strtod(token, &end); int active;
                if (end == token || rate <= 0 || strspn(end, "*+") != strlen(end)) continue;
                active = strchr(end, '*') != NULL; *end = 0;
                DisplayMode *m = &d->modes[d->mode_count++];
                copystr(m->output, sizeof m->output, current); memcpy(m->mode, name, strlen(name) + 1);
                copystr(m->rate, sizeof m->rate, token); m->current = active;
            }
        }
    }
    if (!wallpaper_dir(dir, sizeof dir) || !(dp = opendir(dir))) return;
    d->wallpaper_available = 1;
    while ((entry = readdir(dp))) {
        char path[PATH_MAX]; struct stat st;
        if (entry->d_name[0] == '.' || !wallpaper_ext(entry->d_name)) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, entry->d_name) >= (int)sizeof path || stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        if (d->wallpaper_count < DESKTOP_WALLPAPERS)
            copystr(d->wallpaper_names[d->wallpaper_count++], sizeof d->wallpaper_names[0], entry->d_name);
    }
    closedir(dp);
    qsort(d->wallpaper_names, d->wallpaper_count, sizeof d->wallpaper_names[0], wallpaper_cmp);
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
    char out[MAX_OUTPUT], path[PATH_MAX], dir[PATH_MAX];
    if (r->type == ACT_DISPLAY_APPLY) {
        char mode[64], rate[32], extra; char *argv[] = {"xrandr", "--output", r->arg, "--mode", mode, "--rate", rate, NULL};
        if (strpbrk(r->arg, " \t\r\n") || sscanf(r->password, "%63s %31s %c", mode, rate, &extra) != 2) return -1;
        if (run_command(argv, NULL, out, sizeof out, 10000)) return -1;
        if (!desktop_save_mode(r->arg, mode, rate)) { copystr(message, cap, "Display applied, but could not save for restart."); return 1; }
        copystr(message, cap, "Display mode applied and saved."); return 0;
    }
    if (r->type == ACT_WALLPAPER_APPLY) {
        char *argv[] = {"feh", "--bg-fill", "--", path, NULL}; struct stat st;
        if (!r->password[0] || r->password[0] == '.' || strchr(r->password, '/') || !wallpaper_ext(r->password) ||
            !wallpaper_dir(dir, sizeof dir) || snprintf(path, sizeof path, "%s/%s", dir, r->password) >= (int)sizeof path ||
            stat(path, &st) || !S_ISREG(st.st_mode) || run_command(argv, NULL, out, sizeof out, 15000)) return -1;
        copystr(message, cap, "Wallpaper applied. Choose another any time."); return 0;
    }
    return -1;
}
