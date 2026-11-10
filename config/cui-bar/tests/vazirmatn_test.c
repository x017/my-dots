#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
int main(int argc, char **argv) {
    cui_sw_renderer *r = cui_sw_create(); cui_renderer_iface iface = cui_sw_iface(r);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = iface, .theme = CUI_THEME_DARK});
    const char *path = getenv("VAZIRMATN_TEST_FONT");
    cui_font expected;
    float original, actual, preferred;
    char tmp[] = "/tmp/cui-font-test-XXXXXX", script[256]; FILE *f;
    assert(argc == 2 && path && ui && mkdtemp(tmp));
    snprintf(script, sizeof script, "%s/fc-match", tmp);
    f = fopen(script, "w"); assert(f);
    fputs("#!/bin/sh\ncase \"$*\" in\n"
          " *Vazirmatn*|*Vazir:*) printf '%s\\n%s\\n' \"${FAKE_FAMILY:-Vazirmatn}\" \"$VAZIRMATN_TEST_FONT\" ;;\n"
          " *) printf '%s\\n' \"$VAZIRMATN_TEST_FONT\" ;;\nesac\n", f);
    assert(!fclose(f) && !chmod(script, 0700) && !setenv("PATH", tmp, 1));
    if (getenv("TEST_SIGCHLD_IGNORED")) signal(SIGCHLD, SIG_IGN);
    original = cui_measure_text(ui, CUI_FONT_MONO, 24, "سلام Persian test", -1).width;
    expected = cui_font_load_file(ui, path); assert(expected >= 0);
    preferred = cui_measure_text(ui, expected, 24, "سلام Persian test", -1).width;
    cui_persian_fallback(ui, !strcmp(argv[1], "override"));
    actual = cui_measure_text(ui, CUI_FONT_MONO, 24, "سلام Persian test", -1).width;
    if (!strcmp(argv[1], "preferred")) assert(fabsf(actual - preferred) < .01f);
    else if (!strcmp(argv[1], "override")) assert(fabsf(actual - preferred) > .1f);
    else if (!strcmp(argv[1], "missing")) assert(fabsf(actual - preferred) > .1f);
    else assert(0);
    assert(original >= 0);
    cui_destroy(ui); cui_sw_destroy(r); unlink(script); rmdir(tmp);
    puts("Persian font selection and custom override passed");
    return 0;
}
