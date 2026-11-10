/* Exercise the shape and outline construction against the same mock bitmap. */
#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
#define W 100
#define H 100
static unsigned char maps[4][H][W];
static int next_map, foreground;
Pixmap XCreatePixmap(Display *d, Drawable window, unsigned int w, unsigned int h, unsigned int depth) {
    (void)d; (void)window; assert(w == W && h == H && depth == 1); return ++next_map;
}
GC XCreateGC(Display *d, Drawable draw, unsigned long mask, XGCValues *v) {
    (void)d; (void)draw; (void)mask; (void)v; return (GC)1;
}
int XFreeGC(Display *d, GC gc) { (void)d; (void)gc; return 0; }
int XSetForeground(Display *d, GC gc, unsigned long pixel) { (void)d; (void)gc; foreground = (int)pixel; return 0; }
int XFillRectangle(Display *d, Drawable p, GC gc, int x, int y, unsigned int w, unsigned int h) {
    (void)d; (void)gc;
    for (int j = y; j < y + (int)h; j++) for (int i = x; i < x + (int)w; i++)
        if (i >= 0 && i < W && j >= 0 && j < H) maps[p][j][i] = foreground;
    return 0;
}
int XFillArc(Display *d, Drawable p, GC gc, int x, int y, unsigned int w, unsigned int h, int a, int b) {
    (void)d; (void)gc; (void)a; (void)b;
    if (!w || !h) return 0;
    for (int j = y; j < y + (int)h; j++) for (int i = x; i < x + (int)w; i++) {
        double dx = (i + .5 - (x + w / 2.0)) / (w / 2.0);
        double dy = (j + .5 - (y + h / 2.0)) / (h / 2.0);
        if (i >= 0 && i < W && j >= 0 && j < H && dx*dx + dy*dy <= 1) maps[p][j][i] = foreground;
    }
    return 0;
}
int main(void) {
    Bar b = {0};
    Pixmap shape = rounded_mask(&b, 1, W, H, POPUP_RADIUS, 0);
    Pixmap outline = rounded_mask(&b, 1, W, H, POPUP_RADIUS, POPUP_BORDER);
    assert(shape && outline);
    for (int y = 0; y < H; y++) {
        int count = 0;
        for (int x = 0; x < W; x++) {
            assert(!maps[outline][y][x] || maps[shape][y][x]);
            count += maps[outline][y][x];
        }
        assert(count > 0); /* no missing rows, including all four corners */
    }
    for (int x = 0; x < W; x++) {
        int count = 0;
        for (int y = 0; y < H; y++) count += maps[outline][y][x];
        assert(count > 0);
    }
    assert(!maps[outline][H/2][W/2]);
    assert(maps[outline][0][W/2] && maps[outline][H-1][W/2]);
    puts("outline shares the window shape and is continuous around corners");
    return 0;
}
