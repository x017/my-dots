#define main bar_main
#include "../bar.c"
#undef main
#include <assert.h>
static void frame(cui_ctx *ui, UIState *st) {
 cui_begin_frame(ui,.016f);
 cui_column(ui,.width=CUI_FILL,.height=CUI_FILL,.pad=18,.gap=14) {
   cui_text(ui,"Control center",.font=CUI_FONT_MEDIUM,.size=18);
   dashboard_nav(ui,st);
 }
 cui_end_frame(ui);
}
int main(void) {
 cui_sw_renderer *r=cui_sw_create();cui_renderer_iface iface=cui_sw_iface(r);cui_ctx *ui=cui_create(&(cui_ctx_desc){.renderer=iface,.theme=CUI_THEME_DARK});UIState st={.page=4};
 cui_set_viewport(ui,490,650,1);frame(ui,&st);
 cui_input_mouse_move(ui,145,62);cui_input_mouse_button(ui,CUI_MOUSE_LEFT,true);frame(ui,&st);
 cui_input_mouse_button(ui,CUI_MOUSE_LEFT,false);frame(ui,&st);
 assert(st.page==0);puts("navigation click passed");cui_destroy(ui);cui_sw_destroy(r);return 0;
}
