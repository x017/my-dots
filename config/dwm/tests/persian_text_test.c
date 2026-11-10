#include "../persian_text.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    char *s;
    assert(persian_visual_text("Hello world 123") == NULL);
    assert(persian_visual_text("bad\xff") == NULL);
    s = persian_visual_text("سلام"); assert(s);
    /* FriBidi outputs joined presentation forms in visual order, not the
       original isolated logical characters. */
    assert(strcmp(s, "سلام") != 0 && !strstr(s, "س")); free(s);
    s = persian_visual_text("پنجره 123 - music"); assert(s);
    assert(strstr(s, "123") && strstr(s, "music")); free(s);
    s = persian_visual_text("می\u200cروم"); assert(s && !strstr(s, "\u200c")); free(s);
    puts("Persian joining, mixed-direction text, joiner and invalid UTF-8 passed");
    return 0;
}
