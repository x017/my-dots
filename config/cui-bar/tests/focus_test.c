#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
static Window current_focus; static int set_calls;
int XGetInputFocus(Display *d, Window *focus, int *revert) {
    (void)d; *focus = current_focus; *revert = RevertToParent; return Success;
}
int XSetInputFocus(Display *d, Window focus, int revert, Time t) {
    (void)d; (void)revert; (void)t; current_focus = focus; set_calls++; return Success;
}
int main(void) {
    Bar b = {0}; b.popup = 100; current_focus = 200;
    focus_popup(&b); assert(current_focus == 100 && b.popup_previous_focus == 200 && set_calls == 1);
    focus_popup(&b); assert(b.popup_previous_focus == 200 && set_calls == 1);
    restore_popup_focus(&b); assert(current_focus == 200 && set_calls == 2);
    focus_popup(&b); current_focus = 300; restore_popup_focus(&b);
    assert(current_focus == 300 && set_calls == 3);
    puts("popup focus restoration and window switching passed"); return 0;
}
