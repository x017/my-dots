/* Exercises the real .desktop menu reader and the Desktop Entry Exec parser
 * against fixture files in a temporary XDG data tree. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../dashboard/launcher_exec.h"
#include "../dashboard/launcher_apps.h"

static char root[64], userdir[96], sysdir[96];

static void write_entry(const char *dir, const char *id, const char *body) {
    char path[160];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, id);
    f = fopen(path, "w");
    assert(f && fputs(body, f) != EOF && !fclose(f));
}

static const LauncherApp *find(const LauncherAppList *list, const char *name) {
    int i;
    for (i = 0; i < list->count; i++) if (!strcmp(list->items[i].name, name)) return &list->items[i];
    return NULL;
}

static void check_exec(const char *exec, const char *icon, int expected, ...) {
    char argv[LAUNCHER_ARGV][LAUNCHER_ARG];
    va_list want;
    int count = launcher_exec_argv(exec, "Firefox", icon, "/x/firefox.desktop", argv, LAUNCHER_ARGV);
    int i;
    assert(count == expected);
    va_start(want, expected);
    for (i = 0; i < expected; i++) {
        const char *w = va_arg(want, const char *);
        assert(!strcmp(argv[i], w));
    }
    va_end(want);
}

int main(void) {
    LauncherAppList list;
    const LauncherApp *app;
    char argv[LAUNCHER_ARGV][LAUNCHER_ARG], long_token[400];

    /* Exec parsing: field codes, quoting, escapes and malformed lines. */
    check_exec("/usr/lib/firefox/firefox %u", NULL, 1, "/usr/lib/firefox/firefox");
    check_exec("app %i %c %k %%x", "firefox", 6, "app", "--icon", "firefox", "Firefox",
               "/x/firefox.desktop", "%x");
    check_exec("app %i", NULL, 1, "app");                    /* no Icon: %i vanishes */
    check_exec("echo \"a b\" c\\sd", NULL, 3, "echo", "a b", "c d");
    check_exec("  spaced   args\t", NULL, 2, "spaced", "args");
    check_exec("x \\101\\z", NULL, 2, "x", "Az");            /* octal and reserved escape */
    assert(launcher_exec_argv("unbalanced \"quote", NULL, NULL, NULL, argv, LAUNCHER_ARGV) == -1);
    assert(launcher_exec_argv(NULL, NULL, NULL, NULL, argv, LAUNCHER_ARGV) == -1);
    assert(launcher_exec_argv("", NULL, NULL, NULL, argv, LAUNCHER_ARGV) == 0);
    memset(long_token, 'a', sizeof long_token - 1);
    long_token[sizeof long_token - 1] = '\0';
    assert(launcher_exec_argv(long_token, NULL, NULL, NULL, argv, LAUNCHER_ARGV) == -1);

    /* Menu reading from a temporary XDG tree. */
    strcpy(root, "/tmp/dwm-launcher-XXXXXX");
    assert(mkdtemp(root));
    snprintf(userdir, sizeof userdir, "%s/user", root);
    snprintf(sysdir, sizeof sysdir, "%s/system", root);
    assert(mkdir(userdir, 0700) == 0 && mkdir(sysdir, 0700) == 0);
    strcat(userdir, "/applications");
    strcat(sysdir, "/applications");
    assert(mkdir(userdir, 0700) == 0 && mkdir(sysdir, 0700) == 0);
    write_entry(userdir, "firefox.desktop",
        "[Desktop Entry]\nType=Application\nName=Firefox\nGenericName=Web Browser\n"
        "Name[fa]=\xd9\x81\xd8\xa7\xdb\x8c\xd8\xb1\xd9\x81\xd8\xa7\xda\xa9\xd8\xb3\n"
        "Exec=/usr/lib/firefox/firefox %u\nIcon=firefox\nTerminal=false\n"
        "Categories=Network;WebBrowser;\n");
    write_entry(userdir, "htop.desktop",
        "[Desktop Entry]\nType=Application\nName=Htop\nExec=htop\nIcon=htop\n"
        "Terminal=true\nCategories=System;ConsoleOnly;\n");
    write_entry(userdir, "odd.desktop",
        "[Desktop Entry]\nType=Application\nName=Odd\\smulti  line\nExec=echo \"a b\" c\\sd\n"
        "Categories=Utility;\n[Desktop Action New]\nName=Ignored group\nExec=ignored\n");
    write_entry(userdir, "tryexec-missing.desktop",
        "[Desktop Entry]\nType=Application\nName=Missing\nExec=missing\nTryExec=no-such-tool-xyz\n");
    write_entry(userdir, "tryexec-present.desktop",
        "[Desktop Entry]\nType=Application\nName=Shell\nExec=sh\nTryExec=/bin/sh\nCategories=System;\n");
    write_entry(userdir, "hidden.desktop",
        "[Desktop Entry]\nType=Application\nName=Hidden\nExec=hidden\nNoDisplay=true\n");
    write_entry(userdir, "link.desktop", "[Desktop Entry]\nType=Link\nName=Link\nURL=http://x\n");
    write_entry(userdir, "noentry.desktop", "Name=Orphan\nExec=orphan\n");
    write_entry(userdir, "firefox.desktop.duplicate-check", "not a desktop file\n");
    /* The same id in the system directory must lose to the user's copy. */
    write_entry(sysdir, "firefox.desktop",
        "[Desktop Entry]\nType=Application\nName=System Firefox\nExec=system-firefox\n");
    write_entry(sysdir, "only-system.desktop",
        "[Desktop Entry]\nType=Application\nName=Inkscape\nExec=inkscape %F\nCategories=Graphics;VectorGraphics;\n");

    unsetenv("LANG"); unsetenv("LC_ALL"); unsetenv("LC_MESSAGES");
    userdir[strlen(userdir) - strlen("/applications")] = '\0';
    sysdir[strlen(sysdir) - strlen("/applications")] = '\0';
    setenv("XDG_DATA_HOME", userdir, 1);
    setenv("XDG_DATA_DIRS", sysdir, 1);
    launcher_apps_read(&list);

    assert(list.count == 5);
    /* Sorted by category, then name. */
    assert(!strcmp(list.items[0].group, "Graphics") && !strcmp(list.items[0].name, "Inkscape"));
    assert(!strcmp(list.items[1].group, "Network") && !strcmp(list.items[1].name, "Firefox"));
    app = find(&list, "Firefox");
    assert(app && !strcmp(app->exec, "/usr/lib/firefox/firefox %u") && !strcmp(app->binary, "firefox") &&
           !strcmp(app->icon, "firefox") && !app->terminal);
    assert(strstr(app->desktop, "/user/applications/firefox.desktop"));
    assert(!find(&list, "System Firefox") && !find(&list, "Hidden") && !find(&list, "Link") &&
           !find(&list, "Orphan") && !find(&list, "Missing"));
    assert(find(&list, "Shell"));
    app = find(&list, "Htop");
    assert(app && app->terminal && !strcmp(app->group, "System") && !strcmp(app->binary, "htop"));
    app = find(&list, "Odd multi  line");
    assert(app && !strcmp(app->group, "Utility"));
    app = find(&list, "Inkscape");
    assert(app && !strcmp(app->binary, "inkscape"));

    /* A Persian session prefers Name[fa]. */
    setenv("LANG", "fa_IR.UTF-8", 1);
    launcher_apps_read(&list);
    app = find(&list, "\xd9\x81\xd8\xa7\xdb\x8c\xd8\xb1\xd9\x81\xd8\xa7\xda\xa9\xd8\xb3");
    assert(app && !strcmp(app->exec, "/usr/lib/firefox/firefox %u"));
    assert(!find(&list, "Firefox"));

    snprintf(long_token, sizeof long_token, "rm -rf %s", root);
    assert(system(long_token) == 0);
    puts("desktop menu reading, localisation and Exec parsing passed");
    return 0;
}
