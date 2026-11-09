/* Independent, bounded playerctl and dunstctl worker. No D-Bus name is claimed. */
#define ISLAND_NOTICES 8
#define ISLAND_JSON_TOKENS 16384
#define ISLAND_JSON_CAP 262144

typedef struct { int id; char app[96], title[192], body[384], icon_path[512]; } IslandNotice;
typedef struct {
    int player_available, playing, notifications_available, history_count, active_count, notice_count;
    char artist[160], title[192], art_url[1024], player[128], track_id[256];
    double duration, position; int position_available; char loop[16]; int shuffle;
    IslandNotice notices[ISLAND_NOTICES];
} IslandFrame;
enum { ISLAND_PREVIOUS = 1, ISLAND_TOGGLE, ISLAND_NEXT, ISLAND_DISMISS, ISLAND_SEEK, ISLAND_SHUFFLE, ISLAND_LOOP };
typedef struct { int action, id; char player[128], track_id[256], title[192], artist[160]; } IslandRequest;

/* Minimal bounded JSON token tree for busctl's typed variant output. */
typedef struct { int start, end, parent, next; } IslandToken;
static int island_json(const char *s, IslandToken *t, int cap) {
    int i = 0, n = 0, parent = -1, stack[64], depth = 0;
    while (s[i]) {
        int start, id; char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ':' || c == ',') { i++; continue; }
        if (c == '}' || c == ']') {
            if (!depth || (c == '}' ? s[t[stack[depth-1]].start] != '{' : s[t[stack[depth-1]].start] != '[')) return -1;
            id = stack[--depth]; t[id].end = ++i; t[id].next = n;
            parent = depth ? stack[depth-1] : -1; continue;
        }
        if (n >= cap) return -1;
        id = n++; t[id] = (IslandToken){ .start = i, .end = -1, .parent = parent, .next = n };
        if (c == '{' || c == '[') {
            if (depth >= 64) return -1;
            stack[depth++] = id; parent = id; i++; continue;
        }
        if (c == '"') {
            i++;
            while (s[i] && s[i] != '"') {
                if ((unsigned char)s[i] < 0x20) return -1;
                if (s[i++] == '\\') { if (!s[i]) return -1; i++; }
            }
            if (!s[i]) return -1;
            t[id].end = ++i; continue;
        }
        start = i;
        while (s[i] && s[i] != ',' && s[i] != ']' && s[i] != '}' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
        if (i == start) return -1;
        t[id].end = i;
    }
    return depth ? -1 : n;
}
static int island_key(const char *s, const IslandToken *t, int object, int n, const char *key) {
    int i; size_t len = strlen(key);
    if (object < 0 || object >= n || s[t[object].start] != '{') return -1;
    for (i = object + 1; i < t[object].next && i < n;) {
        int v = i + 1;
        if (v >= n || t[i].parent != object || t[v].parent != object) return -1;
        if (s[t[i].start] == '"' && t[i].end - t[i].start == (int)len + 2 && !strncmp(s + t[i].start + 1, key, len)) return v;
        i = t[v].next;
    }
    return -1;
}
static int island_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void island_string(const char *s, const IslandToken *t, int id, char *out, size_t cap) {
    int i, end; size_t used = 0;
    if (!cap) return;
    out[0] = 0;
    if (id < 0 || s[t[id].start] != '"') return;
    i = t[id].start + 1; end = t[id].end - 1;
    while (i < end && used + 1 < cap) {
        unsigned char c = (unsigned char)s[i++];
        if (c == '\\' && i < end) {
            c = (unsigned char)s[i++];
            if (c == 'u' && i + 4 <= end) {
                int j, cp = 0, v;
                for (j = 0; j < 4; j++) { v = island_hex(s[i + j]); if (v < 0) break; cp = cp * 16 + v; }
                if (j != 4) break;
                i += 4;
                if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= end && s[i] == '\\' && s[i+1] == 'u') {
                    int low = 0; for (j = 0; j < 4; j++) { v = island_hex(s[i + 2 + j]); if (v < 0) break; low = low * 16 + v; }
                    if (j == 4 && low >= 0xdc00 && low <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00; i += 6; }
                }
                if (cp < 0x20 || (cp >= 0xd800 && cp <= 0xdfff)) cp = ' ';
                if (cp < 0x80) out[used++] = (char)cp;
                else if (cp < 0x800 && used + 2 < cap) { out[used++] = (char)(0xc0 | cp >> 6); out[used++] = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000 && used + 3 < cap) { out[used++] = (char)(0xe0 | cp >> 12); out[used++] = (char)(0x80 | ((cp >> 6) & 63)); out[used++] = (char)(0x80 | (cp & 63)); }
                else if (cp >= 0x10000 && used + 4 < cap) { out[used++] = (char)(0xf0 | cp >> 18); out[used++] = (char)(0x80 | ((cp >> 12) & 63)); out[used++] = (char)(0x80 | ((cp >> 6) & 63)); out[used++] = (char)(0x80 | (cp & 63)); }
                continue;
            }
            if (c == 'n' || c == 'r' || c == 't' || c == 'b' || c == 'f') c = ' ';
        }
        if (c < 0x20) c = ' ';
        out[used++] = (char)c;
    }
    out[used] = 0;
}
static void island_variant_string(const char *s, IslandToken *t, int n, int object, const char *key, char *out, size_t cap) {
    int v = island_key(s, t, object, n, key);
    if (v >= 0) v = island_key(s, t, v, n, "data");
    if (v >= 0) island_string(s, t, v, out, cap);
}
static void island_history(const char *json, IslandFrame *frame) {
    IslandToken *t = calloc(ISLAND_JSON_TOKENS, sizeof *t); int n, root, array, i;
    frame->notice_count = 0;
    if (!t) return;
    n = island_json(json, t, ISLAND_JSON_TOKENS);
    if (n < 1) { free(t); return; }
    root = island_key(json, t, 0, n, "data");
    if (root < 0 || json[t[root].start] != '[') { free(t); return; }
    array = root + 1;
    if (array >= n || t[array].parent != root || json[t[array].start] != '[') { free(t); return; }
    for (i = array + 1; i < t[array].next && i < n && frame->notice_count < ISLAND_NOTICES; i = t[i].next) {
        IslandNotice *notice;
        if (t[i].parent != array || json[t[i].start] != '{') break;
        notice = &frame->notices[frame->notice_count++]; memset(notice, 0, sizeof *notice);
        {
            int v = island_key(json, t, i, n, "id");
            char *end; long id;
            if (v >= 0) v = island_key(json, t, v, n, "data");
            if (v >= 0) {
                errno = 0; id = strtol(json + t[v].start, &end, 10);
                if (!errno && end == json + t[v].end && id > 0 && id <= INT_MAX) notice->id = (int)id;
            }
        }
        island_variant_string(json, t, n, i, "icon_path", notice->icon_path, sizeof notice->icon_path);
        island_variant_string(json, t, n, i, "appname", notice->app, sizeof notice->app);
        island_variant_string(json, t, n, i, "summary", notice->title, sizeof notice->title);
        island_variant_string(json, t, n, i, "body", notice->body, sizeof notice->body);
    }
    free(t);
}
/* Split metadata fields without collapsing empty tab-separated fields. */
static void island_metadata(char *out, IslandFrame *frame) {
    char *f[9] = {0}, *p = out, *end; int i; double n;
    for (i = 0; i < 9; i++) {
        f[i] = p; end = strchr(p, '\t');
        if (!end) break;
        *end = 0; p = end + 1;
    }
    if (i < 8) return;
    f[8][strcspn(f[8], "\r\n")] = 0;
    copystr(frame->artist, sizeof frame->artist, f[0]);
    copystr(frame->title, sizeof frame->title, f[1]);
    copystr(frame->art_url, sizeof frame->art_url, f[2]);
    copystr(frame->player, sizeof frame->player, f[5]);
    copystr(frame->track_id, sizeof frame->track_id, f[6]);
    copystr(frame->loop, sizeof frame->loop, f[7]);
    frame->shuffle = !strcasecmp(f[8], "true") || !strcmp(f[8], "1");
    errno = 0; n = strtod(f[3], &end);
    if (!errno && end != f[3] && !*end && isfinite(n) && n > 0 && n <= 604800000000.0) frame->duration = n / 1000000.0;
    errno = 0; n = strtod(f[4], &end);
    if (!errno && end != f[4] && !*end && isfinite(n) && n >= 0 && n <= 604800000000.0) {
        frame->position = n / 1000000.0; frame->position_available = 1;
        if (frame->duration > 0 && frame->position > frame->duration) frame->position = frame->duration;
    }
}
static void island_sample(IslandFrame *frame) {
    char out[4096]; char *json;
    char *status[] = {"playerctl", "status", NULL};
    char *metadata[] = {"playerctl", "metadata", "--format", "{{artist}}\t{{title}}\t{{mpris:artUrl}}\t{{mpris:length}}\t{{position}}\t{{playerInstance}}\t{{mpris:trackid}}\t{{loop}}\t{{shuffle}}", NULL};
    char *count[] = {"dunstctl", "count", NULL};
    char *history[] = {"dunstctl", "history", NULL};
    memset(frame, 0, sizeof *frame);
    if (!run_command(status, NULL, out, sizeof out, 800) && (!strncmp(out, "Playing", 7) || !strncmp(out, "Paused", 6))) {
        frame->player_available = 1; frame->playing = !strncmp(out, "Playing", 7);
        if (!run_command(metadata, NULL, out, sizeof out, 800)) {
            island_metadata(out, frame);
        }
        if (!frame->title[0]) copystr(frame->title, sizeof frame->title, "Unknown track");
    }
    if (!run_command(count, NULL, out, sizeof out, 1500)) {
        char *history_line = strstr(out, "History:"), *waiting_line = strstr(out, "Waiting:");
        char *displayed_line = strstr(out, "Currently displayed:");
        int history_num, waiting = 0, displayed = 0;
        if (history_line && sscanf(history_line, "History: %d", &history_num) == 1 && history_num >= 0) {
            if (waiting_line && sscanf(waiting_line, "Waiting: %d", &waiting) != 1) waiting = 0;
            if (displayed_line && sscanf(displayed_line, "Currently displayed: %d", &displayed) != 1) displayed = 0;
            if (waiting < 0) waiting = 0;
            if (displayed < 0) displayed = 0;
            frame->notifications_available = 1; frame->history_count = history_num;
            frame->active_count = waiting > INT_MAX - displayed ? INT_MAX : waiting + displayed;
            if (history_num && (json = malloc(ISLAND_JSON_CAP))) {
                if (!run_command(history, NULL, json, ISLAND_JSON_CAP, 1200)) island_history(json, frame);
                free(json);
            }
        }
    }
}
static void island_worker(int fd) {
    IslandFrame frame; struct pollfd p = {.fd = fd, .events = POLLIN};
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int ready; IslandRequest request; char out[256];
        island_sample(&frame);
        if (send(fd, &frame, sizeof frame, MSG_NOSIGNAL) != sizeof frame) break;
        /* Faster updates without polling on the X11 render thread. */
        ready = poll(&p, 1, 500);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (ready && (p.revents & POLLIN)) {
            if (recv(fd, &request, sizeof request, 0) != sizeof request) break;
            if (request.action >= ISLAND_PREVIOUS && request.action <= ISLAND_NEXT) {
                char *command = request.action == ISLAND_PREVIOUS ? "previous" : request.action == ISLAND_TOGGLE ? "play-pause" : "next";
                char *selected[] = {"playerctl", "--player", request.player, command, NULL};
                char *fallback[] = {"playerctl", command, NULL};
                run_command(request.player[0] ? selected : fallback, NULL, out, sizeof out, 800);
            } else if ((request.action == ISLAND_SHUFFLE || request.action == ISLAND_LOOP) && request.player[0] && !strcmp(request.player, frame.player)) {
                char *command = request.action == ISLAND_SHUFFLE ? "shuffle" : "loop";
                char *value = request.action == ISLAND_SHUFFLE ? "toggle" : !strcasecmp(frame.loop, "None") ? "Playlist" : !strcasecmp(frame.loop, "Playlist") ? "Track" : "None";
                char *argv[] = {"playerctl", "--player", request.player, command, value, NULL};
                if (run_command(argv, NULL, out, sizeof out, 800)) fprintf(stderr, "cui-bar: player option is unsupported\n");
            } else if (request.action == ISLAND_SEEK && request.id >= 0 && frame.player_available && frame.duration > 0 &&
                       request.id <= frame.duration && frame.player[0] && !strcmp(request.player, frame.player) &&
                       !strcmp(request.track_id, frame.track_id) && !strcmp(request.title, frame.title) && !strcmp(request.artist, frame.artist)) {
                char seconds[32]; char *argv[] = {"playerctl", "--player", request.player, "position", seconds, NULL};
                snprintf(seconds, sizeof seconds, "%d", request.id);
                if (run_command(argv, NULL, out, sizeof out, 800)) fprintf(stderr, "cui-bar: player seek failed or is unsupported\n");
            } else if (request.action == ISLAND_DISMISS && request.id > 0) {
                char id[32]; char *argv[] = {"dunstctl", "history-rm", id, NULL};
                snprintf(id, sizeof id, "%d", request.id);
                if (run_command(argv, NULL, out, sizeof out, 1200))
                    fprintf(stderr, "cui-bar: could not remove notification %s from Dunst history\n", id);
            }
        }
    }
    close(fd); _exit(0);
}
