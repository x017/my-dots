#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
int main(void) {
    const char *sample = "{\"type\":\"method_call\",\"member\":\"Notify\",\"payload\":{\"data\":[\"Telegram\",0,\"\",\"Alice\",\"Hello\",[],{\"image-data\":{\"type\":\"(iiibiiay)\",\"data\":[2,1,8,true,8,4,[255,0,0,255,0,0,255,255]]}},4000]}}";
    LiveNotice notice;
    assert(live_parse(sample, &notice));
    assert(!strcmp(notice.title, "Alice") && !strcmp(notice.body, "Hello"));
    assert(notice.has_image && notice.rgba[0] == 255 && notice.rgba[2] == 0);
    assert(notice.rgba[39*4] == 0 && notice.rgba[39*4+2] == 255);
    assert(!live_parse("{\"type\":\"signal\",\"member\":\"Notify\"}", &notice));
    assert(!live_parse("{broken", &notice));
    if (getenv("TEST_LIVE_BUS")) {
        int pair[2], status; pid_t worker, sender; struct pollfd poller;
        struct timespec delay = {0, 400000000};
        assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
        worker = fork(); assert(worker >= 0);
        if (!worker) { close(pair[0]); live_worker(pair[1]); }
        close(pair[1]); nanosleep(&delay, NULL);
        sender = fork(); assert(sender >= 0);
        if (!sender) {
            int nullfd = open("/dev/null", O_WRONLY);
            dup2(nullfd, 2); close(nullfd);
            execlp("busctl", "busctl", "--user", "call", "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify", "susssasa{sv}i", "Telegram", "0", "", "Alice", "Hello", "0", "1", "image-data", "(iiibiiay)", "2", "1", "8", "true", "8", "4", "8", "255", "0", "0", "255", "0", "0", "255", "255", "4000", (char *)NULL);
            _exit(127);
        }
        poller = (struct pollfd){.fd = pair[0], .events = POLLIN};
        assert(poll(&poller, 1, 5000) == 1);
        assert(recv(pair[0], &notice, sizeof notice, 0) == sizeof notice);
        assert(!strcmp(notice.title, "Alice") && notice.has_image && notice.rgba[0] == 255 && notice.rgba[39*4+2] == 255);
        close(pair[0]); assert(waitpid(sender, &status, 0) == sender); assert(waitpid(worker, &status, 0) == worker);
    }
    puts("live notification parsing and raw image conversion passed");
    return 0;
}
