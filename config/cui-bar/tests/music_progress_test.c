#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
int main(void) {
    int pair[2], i; char buf[sizeof(IslandRequest)]; Bar b = {0};
    cui_sw_renderer *r = cui_sw_create(); cui_renderer_iface iface = cui_sw_iface(r);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = iface, .theme = CUI_THEME_DARK});
    assert(ui && !socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    b.island_fd = pair[0]; b.island.player_available = 1; b.island.position_available = 1;
    b.island.duration = 180.3; strcpy(b.island.player, "test-player");
    cui_set_viewport(ui, 400, 200, 1);
    for (i = 0; i < 20; i++) {
        b.island.position = 42.25 + i * 0.07;
        cui_begin_frame(ui, 0.016f); music_progress(ui, &b); cui_end_frame(ui);
        assert(!b.music_seek_pending);
    }
    errno = 0; assert(recv(pair[1], buf, sizeof buf, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    /* Fractional track ends should not cause a spurious seek either. */
    b.island.position = b.island.duration;
    cui_begin_frame(ui, 0.016f); music_progress(ui, &b); cui_end_frame(ui);
    assert(!b.music_seek_pending);
    assert(recv(pair[1], buf, sizeof buf, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    close(pair[0]); close(pair[1]); cui_destroy(ui); cui_sw_destroy(r);
    puts("music progress does not enqueue unsolicited seeks passed"); return 0;
}
