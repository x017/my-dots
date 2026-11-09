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
    const char *json = "{\"type\":\"aa{sv}\",\"data\":[[{\"id\":{\"type\":\"i\",\"data\":42},\"icon_path\":{\"type\":\"s\",\"data\":\"/tmp/avatar.png\"},\"appname\":{\"type\":\"s\",\"data\":\"Mail\"},\"summary\":{\"type\":\"s\",\"data\":\"Hello \\\"world\\\" \\u263a\"},\"body\":{\"type\":\"s\",\"data\":\"Line 1\\nLine 2\"}},{\"summary\":{\"type\":\"s\",\"data\":\"Another\"}}]]}";
    island_history(json, &f); assert(f.notice_count == 2);
    assert(f.notices[0].id == 42 && f.notices[1].id == 0);
    assert(!strcmp(f.notices[0].icon_path, "/tmp/avatar.png"));
    assert(!strcmp(f.notices[0].app, "Mail"));
    assert(!strcmp(f.notices[0].title, "Hello \"world\" \xe2\x98\xba"));
    assert(!strcmp(f.notices[0].body, "Line 1 Line 2"));
    island_history("{\"data\":[[", &f); assert(f.notice_count == 0);
    assert(mkdtemp(dir));
    script(dir, "playerctl", "#!/bin/sh\ncase \"$1\" in status) echo Playing;; metadata) printf 'Artist\\tTrack\\tfile:///tmp/cover.png\\t180000000\\t45000000\\tspotify\\t/track/1\\tNone\\tfalse\\n';; esac\n");
    script(dir, "dunstctl", "#!/bin/sh\ncase \"$1\" in count) printf 'Waiting: 1\\nCurrently displayed: 2\\nHistory: 1\\n';; history) printf '%s\\n' '{\"type\":\"aa{sv}\",\"data\":[[{\"appname\":{\"type\":\"s\",\"data\":\"Mail\"},\"summary\":{\"type\":\"s\",\"data\":\"New mail\"}}]]}';; esac\n");
    snprintf(path, sizeof path, "%s:/bin:/usr/bin", dir); assert(!setenv("PATH", path, 1));
    island_sample(&f); assert(f.player_available && f.playing && !strcmp(f.artist, "Artist") && !strcmp(f.title, "Track"));
    assert(f.duration == 180 && f.position == 45 && f.position_available && !strcmp(f.player, "spotify"));
    assert(!strcmp(f.art_url, "file:///tmp/cover.png") && !strcmp(f.track_id, "/track/1"));
    { char decoded[128];
      assert(album_path("file:///tmp/a%20b.png", decoded, sizeof decoded) && !strcmp(decoded, "/tmp/a b.png"));
      assert(album_path("file://localhost/tmp/a.png", decoded, sizeof decoded));
      assert(!album_path("https://example.com/image.png", decoded, sizeof decoded));
      assert(!album_path("file:///tmp/bad%00.png", decoded, sizeof decoded)); }
    assert(f.notifications_available && f.history_count == 1 && f.active_count == 3 && f.notice_count == 1 && !strcmp(f.notices[0].title, "New mail"));
    snprintf(logpath, sizeof logpath, "%s/action", dir);
    snprintf(action_script, sizeof action_script, "#!/bin/sh\ncase \"$1\" in status) echo Playing;; metadata) printf 'Artist\\tTrack\\tfile:///tmp/cover.png\\t180000000\\t45000000\\tspotify\\t/track/1\\tNone\\tfalse\\n';; --player) printf '%%s %%s %%s %%s' \"$1\" \"$2\" \"$3\" \"$4\" > '%s';; *) printf '%%s' \"$1\" > '%s';; esac\n", logpath, logpath);
    script(dir, "playerctl", action_script);
    snprintf(action_script, sizeof action_script, "#!/bin/sh\ncase \"$1\" in count) printf 'Waiting: 0\\nCurrently displayed: 0\\nHistory: 0\\n';; history-rm) printf '%%s %%s' \"$1\" \"$2\" > '%s';; esac\n", logpath);
    script(dir, "dunstctl", action_script);
    {
        int pair[2]; IslandRequest action = {ISLAND_NEXT, 0}; pid_t child; struct pollfd poller; FILE *log; char value[32] = {0};
        assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
        child = fork(); assert(child >= 0);
        if (!child) { close(pair[0]); island_worker(pair[1]); }
        close(pair[1]); poller = (struct pollfd){.fd = pair[0], .events = POLLIN};
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        assert(send(pair[0], &action, sizeof action, 0) == sizeof action);
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        log = fopen(logpath, "r"); assert(log); assert(fgets(value, sizeof value, log)); fclose(log); assert(!strcmp(value, "next"));
        action = (IslandRequest){.action = ISLAND_SEEK, .id = 90};
        strcpy(action.player, "spotify"); strcpy(action.track_id, "/track/1"); strcpy(action.title, "Track"); strcpy(action.artist, "Artist");
        assert(send(pair[0], &action, sizeof action, 0) == sizeof action);
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        memset(value, 0, sizeof value); log = fopen(logpath, "r"); assert(log);
        assert(fgets(value, sizeof value, log)); fclose(log); assert(!strcmp(value, "--player spotify position 90"));
        unlink(logpath); strcpy(action.track_id, "/old-track");
        assert(send(pair[0], &action, sizeof action, 0) == sizeof action);
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        assert(access(logpath, F_OK) != 0);
        action = (IslandRequest){.action = ISLAND_SHUFFLE}; strcpy(action.player, "spotify");
        assert(send(pair[0], &action, sizeof action, 0) == sizeof action);
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        memset(value, 0, sizeof value); log = fopen(logpath, "r"); assert(log);
        assert(fgets(value, sizeof value, log)); fclose(log); assert(!strcmp(value, "--player spotify shuffle toggle"));
        action = (IslandRequest){ISLAND_DISMISS, 42};
        assert(send(pair[0], &action, sizeof action, 0) == sizeof action);
        assert(poll(&poller, 1, 5000) == 1 && recv(pair[0], &f, sizeof f, 0) == sizeof f);
        memset(value, 0, sizeof value); log = fopen(logpath, "r"); assert(log);
        assert(fgets(value, sizeof value, log)); fclose(log); assert(!strcmp(value, "history-rm 42"));
        close(pair[0]); assert(waitpid(child, NULL, 0) == child); unlink(logpath);
    }
    assert(!setenv("PATH", "/nonexistent", 1)); island_sample(&f);
    assert(!f.player_available && !f.notifications_available);
    snprintf(path, sizeof path, "%s/playerctl", dir); unlink(path); snprintf(path, sizeof path, "%s/dunstctl", dir); unlink(path); rmdir(dir);
    puts("island JSON, player metadata and notification history passed"); return 0;
}
