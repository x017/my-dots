/* Build with the bar's CUI/X11 headers and -ffunction-sections -Wl,--gc-sections. */
#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>

int main(void) {
    Bar b = {0}; Layout l; int widths[] = {800, 850, 1280, 1920}; size_t i;
    b.tag_count = 9; b.tray_width = 68;
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        b.width = widths[i]; b.current = 0; l = layout(&b);
        assert(l.tray == b.width - RIGHT_MODULES(b.width) - b.tray_width);
        assert(l.audio == l.tray + b.tray_width + 4);
        assert(l.network == l.audio + 66);
        assert(l.bluetooth == l.network + l.network_width + 4);
        assert(l.tags + b.tag_count * 28 <= l.tray - 4);
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
