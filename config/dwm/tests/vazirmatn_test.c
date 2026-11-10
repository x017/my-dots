#include "../drw.c"
#include <assert.h>
int main(int argc, char **argv) {
    FcConfig *config = FcConfigCreate();
    const char *font = getenv("VAZIRMATN_TEST_FONT");
    assert(argc == 2 && config && font);
    if (!strcmp(argv[1], "present")) assert(FcConfigAppFontAddFile(config, (const FcChar8 *)font));
    else assert(!strcmp(argv[1], "missing"));
    assert(FcConfigSetCurrent(config));
    assert(vazirmatn_available() == !strcmp(argv[1], "present"));
    FcConfigDestroy(config);
    puts("native Vazirmatn family detection passed");
    return 0;
}
