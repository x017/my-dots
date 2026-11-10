#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>

static void frame(cui_ctx *ui, Bar *b) {
    cui_begin_frame(ui, .016f);
    popup_contents(ui, b);
    cui_end_frame(ui);
}
static void click(cui_ctx *ui, Bar *b, int x, int y) {
    cui_input_mouse_move(ui, x, y); frame(ui, b);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, b);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, b); frame(ui, b);
}
static void render(cui_ctx *ui, cui_sw_renderer *r, Bar *b, unsigned char *pixels) {
    frame(ui, b);
    cui_sw_render(r, cui_get_draw_data(ui), pixels, b->popup_width, b->popup_height,
                  b->popup_width * 4, CUI_SW_BGRA);
}
static int blue(unsigned char *p) { return p[0] == 0xc9 && p[1] == 0xa8 && p[2] == 0x7f; }
int main(void) {
    const int w = 490, h = 650; Bar b = {0}; unsigned char *pixels = calloc((size_t)w*h, 4);
    cui_sw_renderer *r = cui_sw_create(); cui_renderer_iface iface = cui_sw_iface(r);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = iface, .theme = CUI_THEME_DARK});
    assert(pixels && ui); b.popup_width = w; b.popup_height = h; b.island_fd = -1;
    style(ui); cui_font_set_role(ui, CUI_FONT_BODY, CUI_FONT_MONO); cui_font_set_role(ui, CUI_FONT_MEDIUM, CUI_FONT_MONO);
    cui_set_viewport(ui, w, h, 1); popup_state.page = 4;
    render(ui, r, &b, pixels);
    assert(blue(pixels + 4*(w/2))); assert(blue(pixels + 4*((h-1)*w + w/2)));
    assert(blue(pixels + 4*((h/2)*w))); assert(blue(pixels + 4*((h/2)*w + w-1)));
    click(ui, &b, 72, 142); assert(popup_state.page == 0);
    b.popup_notifications = 1;
    render(ui, r, &b, pixels);
    assert(blue(pixels + 4*(w/2))); assert(blue(pixels + 4*((h-1)*w + w/2)));
    b.popup_calendar = 1; b.popup_notifications = 0;
    render(ui, r, &b, pixels); assert(!blue(pixels + 4*(w/2)));
    cui_destroy(ui); cui_sw_destroy(r); free(pixels);
    puts("dashboard and island outlines, navigation and unchanged calendar passed"); return 0;
}
