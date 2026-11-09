#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
static void script(const char *dir, const char *name, const char *body) {
    char path[512]; FILE *f; snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "w"); assert(f); assert(fputs(body, f) >= 0); fclose(f); assert(!chmod(path, 0700));
}
int main(void) {
    char dir[] = "/tmp/cui-island-test-XXXXXX", path[1024], action_script[2048], logpath[512]; IslandFrame f = {0};
    const char *json = "{\"type\":\"aa{sv}\",\"data\":[[{\"appname\":{\"type\":\"s\",\"data\":\"Mail\"},\"summary\":{\"type\":\"s\",\"data\":\"Hello \\\"world\\\" \\u263a\"},\"body\":{\"type\":\"s\",\"data\":\"Line 1\\nLine 2\"}},{\"summary\":{\"type\":\"s\",\"data\":\"Another\"}}]]}";
    island_history(json, &f); assert(f.notice_count == 2);
    assert(!strcmp(f.notices[0].app, "Mail"));
    assert(!strcmp(f.notices[0].title, "Hello \"world\" \xe2\x98\xba"));
    assert(!strcmp(f.notices[0].body, "Line 1 Line 2"));
    island_history("{\"data\":[[", &f); assert(f.notice_count == 0);
    assert(mkdtemp(dir));
    script(dir, "playerctl", "#!/bin/sh\ncase \"$1\" in status) echo Playing;; metadata) printf 'Artist\\tTrack\\n';; esac\n");
    script(dir, "dunstctl", "#!/bin/sh\ncase \"$1\" in count) printf 'Waiting: 1\\nCurrently displayed: 2\\nHistory: 1\\n';; history) printf '%s\\n' '{\"type\":\"aa{sv}\",\"data\":[[{\"appname\":{\"type\":\"s\",\"data\":\"Mail\"},\"summary\":{\"type\":\"s\",\"data\":\"New mail\"}}]]}';; esac\n");
    snprintf(path, sizeof path, "%s:/bin:/usr/bin", dir); assert(!setenv("PATH", path, 1));
    island_sample(&f); assert(f.player_available && f.playing && !strcmp(f.artist, "Artist") && !strcmp(f.title, "Track"));
    assert(f.notifications_available && f.history_count == 1 && f.active_count == 3 && f.notice_count == 1 && !strcmp(f.notices[0].title, "New mail"));
    snprintf(logpath, sizeof logpath, "%s/action", dir);
    snprintf(action_script, sizeof action_script, "#!/bin/sh\ncase \"$1\" in status) echo Playing;; metadata) printf 'Artist\\tTrack\\n';; *) printf '%%s' \"$1\" > '%s';; esac\n", logpath);
    script(dir, "playerctl", action_script);
    {
        int pair[2], action = ISLAND_NEXT; pid_t child; struct pollfd poller; FILE *log; char value[32] = {0};
        assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
        child = fork(); assert(child >= 0);
        if (!child) { close(pair[0]); island_worker(pair[1]); }
        close(pair[1]); poller = (struct pollfd){.fd = pair[0], .events = POLLIN};
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        assert(send(pair[0], &action, sizeof action, 0) == sizeof action);
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        log = fopen(logpath, "r"); assert(log); assert(fgets(value, sizeof value, log)); fclose(log); assert(!strcmp(value, "next"));
        close(pair[0]); assert(waitpid(child, NULL, 0) == child); unlink(logpath);
    }
    assert(!setenv("PATH", "/nonexistent", 1)); island_sample(&f);
    assert(!f.player_available && !f.notifications_available);
    snprintf(path, sizeof path, "%s/playerctl", dir); unlink(path); snprintf(path, sizeof path, "%s/dunstctl", dir); unlink(path); rmdir(dir);
    puts("island JSON, player metadata and notification history passed"); return 0;
}
