/* Build with the bar's CUI/X11 headers and -ffunction-sections -Wl,--gc-sections. */
#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>

int main(void) {
    Bar b = {0}; Layout l; int widths[] = {800, 850, 1280, 1920}; size_t i;
    {
        unsigned long v[3 + 2 * 9] = {3, 2, 9}; int count, stride;
        v[3 + 8] = 1; v[3 + 9 + 8] = 0;
        assert(workspace_header(v, 3 + 2 * 9, &count, &stride));
        assert(count == 2 && stride == 9 && v[3 + 8] == 1 && v[3 + stride + 8] == 0);
        assert(!workspace_header(v, 3 + 2 * 9 - 1, &count, &stride));
        v[0] = 2; assert(workspace_header(v, 3 + 2 * 8, &count, &stride) && stride == 8);
        v[0] = 4; assert(!workspace_header(v, 3 + 2 * 9, &count, &stride));
        v[0] = 3; v[1] = 17; assert(!workspace_header(v, 3 + 2 * 9, &count, &stride));
    }
    b.tag_count = 9; b.tray_width = 68;
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        b.width = widths[i]; b.current = 0; l = layout(&b);
        assert(l.tray == b.width - RIGHT_MODULES(b.width) - b.tray_width);
        assert(l.dashboard == l.tray + b.tray_width + 4);
        assert(l.audio == l.dashboard + 36);
        assert(l.network == l.audio + 66);
        assert(l.bluetooth == l.network + l.network_width + 4);
        assert(l.tags + b.tag_count * 28 <= l.tray - 4);
        if (l.stats) {
            int stats_x = l.tags + b.tag_count * 28 + 22;
            assert(stats_x - 5 >= l.tags + b.tag_count * 28);
            assert(stats_x + 290 <= l.tray);
        }
        assert(l.lock + 28 <= b.width - PAD);
        if (l.island_width) {
            assert(l.island >= l.tags + b.tag_count * 28 + 8);
            assert(l.island + l.island_width <= l.tray - 6);
            if (l.stats) assert(l.island >= l.tags + b.tag_count * 28 + 22 + 296);
            assert(hover_kind(&b, l.island) == TIP_ISLAND);
            { int x = notification_popup_x(&b, 400);
              assert(x >= 0 && x + 400 <= b.width);
              assert(x + 200 == l.island + l.island_width / 2 || x == 0 || x + 400 == b.width); }
        }
        assert(hover_kind(&b, l.dashboard) == TIP_DASHBOARD);
    }
    b.network_available = 1; b.ethernet = 1;
    assert(!strcmp(network_label(&b), "Ethernet"));
    strcpy(b.snapshot.wifi_device, "wlan0");
    assert(!strcmp(network_label(&b), "Connected"));
    b.snapshot.networks_count = 2;
    strcpy(b.snapshot.networks[0].ssid, "Other");
    strcpy(b.snapshot.networks[1].ssid, "Studio Network"); b.snapshot.networks[1].active = 1;
    assert(!strcmp(network_label(&b), "Studio Network"));
    b.snapshot.wifi_device[0] = 0; b.ethernet = 0;
    assert(!strcmp(network_label(&b), "Offline"));
    puts("bar layout and network label checks passed");
    return 0;
}
