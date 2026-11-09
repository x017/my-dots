#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
static FILE *sample(const char *text) {
    FILE *f = tmpfile(); assert(f); assert(fputs(text, f) >= 0); rewind(f); return f;
}
int main(void) {
    Bar b = {0}; FILE *f; Layout l; int x;
    f = sample("cpu 40 0 0 160 0 0 0 0\ncpu0 10 0 0 90 0 0 0 0\ncpu1 30 0 0 70 0 0 0 0\nintr 0\n");
    read_cpu_stream(&b, f); fclose(f); assert(b.core_count == 2 && b.core_load[0] == -1);
    f = sample("cpu 90 0 0 310 0 0 0 0\ncpu0 30 0 0 170 0 0 0 0\ncpu1 60 0 0 140 0 0 0 0\nintr 0\n");
    read_cpu_stream(&b, f); fclose(f);
    assert(b.cpu == 25 && b.core_load[0] == 20 && b.core_load[1] == 30);
    b.width = 1280; b.tag_count = 9; b.battery = 50; l = layout(&b);
    assert(l.stats); x = l.tags + b.tag_count * 28 + 22;
    assert(hover_kind(&b, x) == TIP_CPU && hover_kind(&b, x + 75) == TIP_MEMORY);
    assert(hover_kind(&b, x + 150) == TIP_TEMP && hover_kind(&b, x + 225) == TIP_BATTERY);
    assert(hover_kind(&b, l.network) == TIP_NETWORK && hover_kind(&b, l.audio) == TIP_SOUND);
    puts("CPU sampling and hover hit tests passed"); return 0;
}
