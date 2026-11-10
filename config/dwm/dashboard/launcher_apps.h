/* Reads the installed .desktop application menu. Free of CUI so the parser can
 * be exercised directly by the test suite. */
#ifndef DWM_LAUNCHER_APPS_H
#define DWM_LAUNCHER_APPS_H
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LAUNCHER_MAX_APPS 512

typedef struct {
    char name[128], exec[512], group[40], binary[48], icon[128], desktop[512];
    int terminal;
} LauncherApp;
typedef struct { LauncherApp items[LAUNCHER_MAX_APPS]; int count; } LauncherAppList;

/* Desktop Entry string values use \s \n \t \r \\ escapes. */
static void launcher_unescape(char *value) {
    char *read = value, *write = value;
    while (*read) {
        if (*read == '\\' && read[1]) {
            read++;
            switch (*read) {
                case 's': *write++ = ' '; break;
                case 'n': *write++ = '\n'; break;
                case 't': *write++ = '\t'; break;
                case 'r': *write++ = '\r'; break;
                case '\\': *write++ = '\\'; break;
                default: *write++ = '\\'; *write++ = *read; break;
            }
            read++;
        } else *write++ = *read++;
    }
    *write = '\0';
}

static int launcher_bool(const char *value) {
    return !strcasecmp(value, "true") || !strcmp(value, "1");
}

/* "fa_IR.UTF-8" yields full="fa_IR" and language="fa", so Name[fa_IR] can be
 * preferred over Name[fa] over Name. */
static void launcher_locale(char *full, size_t full_size, char *language, size_t language_size) {
    const char *env = getenv("LC_ALL");
    size_t n;
    full[0] = language[0] = '\0';
    if (!env || !*env) env = getenv("LC_MESSAGES");
    if (!env || !*env) env = getenv("LANG");
    if (!env || !*env || !strcmp(env, "C") || !strcmp(env, "POSIX")) return;
    n = strcspn(env, ".@");
    if (n >= full_size) n = full_size - 1;
    memcpy(full, env, n); full[n] = '\0';
    n = strcspn(full, "_");
    if (n >= language_size) n = language_size - 1;
    memcpy(language, full, n); language[n] = '\0';
}

static int launcher_executable(const char *program) {
    char path[PATH_MAX], copy[1024], *token, *save;
    const char *dirs = getenv("PATH");
    if (strchr(program, '/')) return access(program, X_OK) == 0;
    if (!dirs || !*dirs) dirs = "/usr/local/bin:/usr/bin:/bin";
    snprintf(copy, sizeof copy, "%s", dirs);
    for (token = strtok_r(copy, ":", &save); token; token = strtok_r(NULL, ":", &save)) {
        if (!*token) continue;
        if (snprintf(path, sizeof path, "%s/%s", token, program) >= (int)sizeof path) continue;
        if (access(path, X_OK) == 0) return 1;
    }
    return 0;
}

/* The first main category, matched on ; boundaries so AudioVideo is not read
 * as Audio. */
static const char *launcher_main_category(const char *categories) {
    static const char *const mains[] = {"AudioVideo", "Audio", "Video", "Development", "Education",
                                        "Game", "Graphics", "Network", "Office", "Science",
                                        "Settings", "System", "Utility", NULL};
    int i;
    for (i = 0; mains[i]; i++) {
        size_t length = strlen(mains[i]);
        const char *p = categories;
        while ((p = strstr(p, mains[i]))) {
            int bounded = (p == categories || p[-1] == ';') && (p[length] == ';' || p[length] == '\0');
            if (bounded) return mains[i];
            p += length;
        }
    }
    return NULL;
}

static int launcher_desktop_parse(const char *path, LauncherApp *app) {
    FILE *file = fopen(path, "r");
    char *line = NULL, full[32], language[8], categories[256] = "", tryexec[256] = "";
    char plain[128] = "", localized[128] = "", language_name[128] = "", generic[128] = "", type[32] = "";
    const char *chosen, *category, *space;
    size_t cap = 0;
    ssize_t length;
    int in_entry = 0, rejected = 0, ok = 0;

    if (!file) return 0;
    memset(app, 0, sizeof *app);
    launcher_locale(full, sizeof full, language, sizeof language);
    while ((length = getline(&line, &cap, file)) > 0) {
        char *key, *value, *equals, *end;
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        key = line;
        while (*key == ' ' || *key == '\t') key++;
        if (!*key || *key == '#') continue;
        if (*key == '[') { in_entry = !strncmp(key, "[Desktop Entry]", 15); continue; }
        if (!in_entry) continue;
        if (!(equals = strchr(key, '='))) continue;
        *equals = '\0';
        end = equals - 1;
        while (end >= key && (*end == ' ' || *end == '\t')) *end-- = '\0';
        value = equals + 1;
        while (*value == ' ' || *value == '\t') value++;
        launcher_unescape(value);
        if (!strcmp(key, "Type")) snprintf(type, sizeof type, "%s", value);
        else if (!strcmp(key, "Exec")) snprintf(app->exec, sizeof app->exec, "%s", value);
        else if (!strcmp(key, "Name")) snprintf(plain, sizeof plain, "%s", value);
        else if (!strcmp(key, "GenericName")) snprintf(generic, sizeof generic, "%s", value);
        else if (!strcmp(key, "Terminal")) app->terminal = launcher_bool(value);
        else if (!strcmp(key, "NoDisplay") || !strcmp(key, "Hidden")) rejected |= launcher_bool(value);
        else if (!strcmp(key, "TryExec")) snprintf(tryexec, sizeof tryexec, "%s", value);
        else if (!strcmp(key, "Categories")) snprintf(categories, sizeof categories, "%s", value);
        else if (!strcmp(key, "Icon")) snprintf(app->icon, sizeof app->icon, "%s", value);
        else if (!strncmp(key, "Name[", 5)) {
            char tag[32];
            snprintf(tag, sizeof tag, "%s", key + 5);
            end = strchr(tag, ']');
            if (end) *end = '\0';
            if (full[0] && !strcmp(tag, full)) snprintf(localized, sizeof localized, "%s", value);
            else if (language[0] && !strcmp(tag, language) && !language_name[0])
                snprintf(language_name, sizeof language_name, "%s", value);
        }
    }
    free(line);
    fclose(file);
    chosen = localized[0] ? localized : language_name[0] ? language_name : plain[0] ? plain : generic;
    if (rejected || strcmp(type, "Application") || !chosen[0] || !app->exec[0]) return 0;
    if (tryexec[0] && (space = strchr(tryexec, ' ')) == NULL && !launcher_executable(tryexec)) return 0;
    snprintf(app->name, sizeof app->name, "%s", chosen);
    snprintf(app->desktop, sizeof app->desktop, "%s", path);
    category = launcher_main_category(categories);
    snprintf(app->group, sizeof app->group, "%s", category ? category : "Applications");
    /* The command's own name, for the hint shown beside each entry. */
    {
        const char *start = app->exec;
        size_t n;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '"') { start++; n = strcspn(start, "\""); }
        else n = strcspn(start, " \t");
        if (n >= sizeof app->binary) n = sizeof app->binary - 1;
        memcpy(app->binary, start, n); app->binary[n] = '\0';
        start = strrchr(app->binary, '/');
        if (start) memmove(app->binary, start + 1, strlen(start));
    }
    ok = app->name[0] && app->exec[0];
    return ok;
}

/* The same desktop file id in an earlier directory wins, as the specification
 * requires, so a user entry in ~/.local/share overrides the system one. */
static int launcher_app_seen(const LauncherAppList *list, const char *id) {
    int i;
    for (i = 0; i < list->count; i++) {
        const char *base = strrchr(list->items[i].desktop, '/');
        base = base ? base + 1 : list->items[i].desktop;
        if (!strcmp(base, id)) return 1;
    }
    return 0;
}

static void launcher_scan_dir(const char *dir, LauncherAppList *list) {
    DIR *handle = opendir(dir);
    struct dirent *entry;
    if (!handle) return;
    while ((entry = readdir(handle)) && list->count < LAUNCHER_MAX_APPS) {
        char path[512];
        LauncherApp app;
        size_t length = strlen(entry->d_name);
        if (length < 9 || strcmp(entry->d_name + length - 8, ".desktop")) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, entry->d_name) >= (int)sizeof path) continue;
        if (launcher_app_seen(list, entry->d_name)) continue;
        if (!launcher_desktop_parse(path, &app)) continue;
        list->items[list->count++] = app;
    }
    closedir(handle);
}

/* Group first, then name, so the palette's section headers stay contiguous. */
static int launcher_app_compare(const void *a, const void *b) {
    const LauncherApp *x = a, *y = b;
    int result = strcasecmp(x->group, y->group);
    return result ? result : strcasecmp(x->name, y->name);
}

static void launcher_apps_read(LauncherAppList *list) {
    const char *home = getenv("HOME"), *dirs = getenv("XDG_DATA_DIRS");
    char path[PATH_MAX], copy[1024], *token, *save;
    list->count = 0;
    {
        const char *base = getenv("XDG_DATA_HOME");
        if (base && *base == '/') snprintf(path, sizeof path, "%s/applications", base);
        else if (home && *home) snprintf(path, sizeof path, "%s/.local/share/applications", home);
        else path[0] = '\0';
        if (path[0]) launcher_scan_dir(path, list);
    }
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    snprintf(copy, sizeof copy, "%s", dirs);
    for (token = strtok_r(copy, ":", &save); token && list->count < LAUNCHER_MAX_APPS;
         token = strtok_r(NULL, ":", &save)) {
        if (!*token) continue;
        if (snprintf(path, sizeof path, "%s/applications", token) >= (int)sizeof path) continue;
        launcher_scan_dir(path, list);
    }
    qsort(list->items, (size_t)list->count, sizeof list->items[0], launcher_app_compare);
}
#endif
