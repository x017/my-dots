#include "../drw.c"
#include <assert.h>
int main(int argc, char **argv) {
    FcConfig *config = FcConfigCreate();
    const char *font = getenv("VAZIRMATN_TEST_FONT");
    assert(argc == 2 && config && font);
    if (!strcmp(argv[1], "present")) assert(FcConfigAppFontAddFile(config, (const FcChar8 *)font));
    else assert(!strcmp(argv[1], "missing"));
    assert(FcConfigSetCurrent(config));
    if (!strcmp(argv[1], "present")) {
        const char *family = getenv("EXPECTED_FONT_PATTERN");
        assert(family && preferred_persian_font() && !strcmp(preferred_persian_font(), family));
    } else assert(!preferred_persian_font());
    FcConfigDestroy(config);
    puts("native Persian font family detection passed");
    return 0;
}
