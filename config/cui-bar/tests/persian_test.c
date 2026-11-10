#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
extern bool cui__text_needs_shaping(cui_ctx *, const char *, int);
static void render(cui_ctx *ui, cui_sw_renderer *r, const char *s, unsigned char *pixels) {
    cui_begin_frame(ui, .016f);
    cui_draw_text(ui, CUI_FONT_MONO, 24, (cui_vec2){5, 10}, CUI_RGB(0xffffff), s, -1);
    cui_end_frame(ui);
    cui_sw_render(r, cui_get_draw_data(ui), pixels, 320, 60, 320 * 4, CUI_SW_BGRA);
}
int main(void) {
    unsigned char *before = calloc(320 * 60, 4), *after = calloc(320 * 60, 4);
    cui_sw_renderer *r = cui_sw_create(); cui_renderer_iface iface = cui_sw_iface(r);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = iface, .theme = CUI_THEME_DARK});
    assert(before && after && ui);
    assert(cui__text_needs_shaping(ui, "سلام دنیا 123", strlen("سلام دنیا 123")));
    assert(!cui__text_needs_shaping(ui, "Hello 123", 9));
    cui_set_viewport(ui, 320, 60, 1);
    render(ui, r, "سلام دنیا", before);
    const char *font = getenv("CUI_PERSIAN_TEST_FONT");
    if (!font) font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    if (access(font, R_OK) != 0) { puts("Set CUI_PERSIAN_TEST_FONT to a Persian-capable font to run this test"); return 77; }
    assert(setenv("CUI_PERSIAN_FONT", font, 1) == 0);
    cui_persian_fallback(ui, 0);
    render(ui, r, "سلام دنیا", after);
    assert(memcmp(before, after, 320 * 60 * 4) != 0);
    assert(cui_measure_text(ui, CUI_FONT_MONO, 24, "سلام دنیا 123", -1).width > 0);
    cui_destroy(ui); cui_sw_destroy(r); free(before); free(after);
    puts("CUI Persian shaping and fallback rendering passed"); return 0;
}
