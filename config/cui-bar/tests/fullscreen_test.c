#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
static unsigned long state[3 + 2 * 9];
static int maps[3], unmaps[3];
int XGetWindowProperty(Display *d, Window w, Atom prop, long off, long len, Bool del, Atom req,
                       Atom *type, int *format, unsigned long *n, unsigned long *left, unsigned char **out) {
    (void)d; (void)w; (void)prop; (void)off; (void)len; (void)del; (void)req;
    *type = XA_CARDINAL; *format = 32; *n = 3 + 2 * 9; *left = 0;
    *out = malloc(sizeof state); assert(*out); memcpy(*out, state, sizeof state); return Success;
}
int XFree(void *p) { free(p); return 0; }
int XMoveResizeWindow(Display *d, Window w, int x, int y, unsigned int width, unsigned int height) {
    (void)d; (void)w; (void)x; (void)y; (void)width; (void)height; return 0;
}
int XMapRaised(Display *d, Window w) { (void)d; assert(w < 3); maps[w]++; return 0; }
int XUnmapWindow(Display *d, Window w) { (void)d; assert(w < 3); unmaps[w]++; return 0; }
int main(void) {
    Bar b = {0}; int i;
    state[0] = 3; state[1] = 2; state[2] = 9;
    b.panel_count = 2;
    for (i = 0; i < 2; i++) {
        int at = 3 + i * 9;
        state[at] = i; state[at + 1] = 1; state[at + 4] = 1000 * i; state[at + 5] = 0;
        state[at + 6] = 1000; state[at + 7] = 800;
        b.panels[i].id = i; b.panels[i].win = i + 1; b.panels[i].x = i * 1000 + SIDE_GAP;
        b.panels[i].y = TOP_GAP; b.panels[i].width = 1000 - SIDE_GAP * 2; b.panels[i].mapped = 1;
    }
    read_workspace(&b); assert(!maps[1] && !maps[2] && !unmaps[1] && !unmaps[2]);
    state[3 + 8] = 1; read_workspace(&b);
    assert(unmaps[1] == 1 && !unmaps[2] && !b.panels[0].mapped && b.panels[1].mapped);
    read_workspace(&b); assert(unmaps[1] == 1 && !maps[1] && !maps[2]);
    state[3 + 8] = 0; state[3 + 9 + 8] = 1; read_workspace(&b);
    assert(maps[1] == 1 && unmaps[2] == 1 && b.panels[0].mapped && !b.panels[1].mapped);
    state[3 + 9 + 8] = 0; read_workspace(&b);
    assert(maps[2] == 1 && b.panels[0].mapped && b.panels[1].mapped);
    puts("per-monitor fullscreen hide and restore passed"); return 0;
}
