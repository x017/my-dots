#define main bar_main
#include "../bar.c"
#undef main
#include <assert.h>
static Snapshot snapshot;
static DesktopState desktop;
static void frame(cui_ctx *ui, UIState *st) {
    cui_begin_frame(ui, .016f);
    dashboard_shell(ui, st, &snapshot, &desktop, false);
    cui_end_frame(ui);
}
static void click(cui_ctx *ui, UIState *st, int x, int y) {
    cui_input_mouse_move(ui, x, y); frame(ui, st);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, st);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, st); frame(ui, st);
}
int main(void) {
    cui_sw_renderer *r = cui_sw_create(); cui_renderer_iface iface = cui_sw_iface(r);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = iface, .theme = CUI_THEME_DARK});
    UIState st = {.page = 4}; style(ui);
    cui_font_set_role(ui, CUI_FONT_BODY, CUI_FONT_MONO);
    cui_font_set_role(ui, CUI_FONT_MEDIUM, CUI_FONT_MONO);
    cui_set_viewport(ui, 490, 650, 1); frame(ui, &st);
    click(ui, &st, 72, 142); assert(st.page == 0);
    click(ui, &st, 72, 250); assert(st.page == 3);
    click(ui, &st, 72, 106); assert(st.page == 4);
    click(ui, &st, 444, 443); assert(st.page == 1);
    cui_set_viewport(ui, 360, 650, 1); st.page = 4; frame(ui, &st);
    click(ui, &st, 112, 73); assert(st.page == 0);
    click(ui, &st, 312, 73); assert(st.page == 3);
    puts("sidebar, compact navigation and overview shortcut passed");
    cui_destroy(ui); cui_sw_destroy(r); return 0;
}
