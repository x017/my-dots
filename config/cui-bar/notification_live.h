/* Observe only notification method calls. Dunst continues to own the service.
 * busctl's JSON stream is parsed in a child; no notification data is logged. */
#define LIVE_IMAGE_SIDE 40
#define LIVE_JSON_CAP (5 * 1024 * 1024)
#define LIVE_TOKEN_CAP 1200000

typedef struct {
    char app[96], title[192], body[384];
    int has_image;
    unsigned char rgba[LIVE_IMAGE_SIDE * LIVE_IMAGE_SIDE * 4];
} LiveNotice;

static int live_number(const char *s, const IslandToken *t, int id, int *out) {
    char *end; long n;
    if (id < 0) return 0;
    errno = 0; n = strtol(s + t[id].start, &end, 10);
    if (errno || end != s + t[id].end || n < 0 || n > INT_MAX) return 0;
    *out = (int)n; return 1;
}
static int live_parse(const char *json, LiveNotice *notice) {
    IslandToken *t = calloc(LIVE_TOKEN_CAP, sizeof *t);
    int n, payload, data, fields[8], i, v, hints, image, arr, a[7], width, height, stride, channels, bits;
    char member[32], type[32];
    if (!t) return 0;
    memset(notice, 0, sizeof *notice);
    n = island_json(json, t, LIVE_TOKEN_CAP);
    if (n < 1) goto fail;
    v = island_key(json, t, 0, n, "member"); if (v < 0) goto fail;
    island_string(json, t, v, member, sizeof member);
    v = island_key(json, t, 0, n, "type"); if (v < 0) goto fail;
    island_string(json, t, v, type, sizeof type);
    if (strcmp(member, "Notify") || strcmp(type, "method_call")) goto fail;
    payload = island_key(json, t, 0, n, "payload");
    data = island_key(json, t, payload, n, "data");
    if (data < 0 || json[t[data].start] != '[') goto fail;
    v = data + 1;
    for (i = 0; i < 8; i++) {
        if (v >= n || t[v].parent != data || v >= t[data].next) goto fail;
        fields[i] = v; v = t[v].next;
    }
    island_string(json, t, fields[0], notice->app, sizeof notice->app);
    island_string(json, t, fields[3], notice->title, sizeof notice->title);
    island_string(json, t, fields[4], notice->body, sizeof notice->body);
    hints = fields[6];
    image = island_key(json, t, hints, n, "image-data");
    if (image < 0) image = island_key(json, t, hints, n, "image_data");
    if (image < 0) image = island_key(json, t, hints, n, "icon_data");
    if (image < 0 || (arr = island_key(json, t, image, n, "data")) < 0 || json[t[arr].start] != '[') goto done;
    v = arr + 1;
    for (i = 0; i < 7; i++) {
        if (v >= n || t[v].parent != arr || v >= t[arr].next) goto done;
        a[i] = v; v = t[v].next;
    }
    if (!live_number(json, t, a[0], &width) || !live_number(json, t, a[1], &height) ||
        !live_number(json, t, a[2], &stride) || !live_number(json, t, a[4], &bits) ||
        !live_number(json, t, a[5], &channels) || width < 1 || height < 1 || width > 1024 || height > 1024 ||
        bits != 8 || (channels != 3 && channels != 4) || stride < width * channels || stride > 4096 ||
        (size_t)stride * height > 4 * 1024 * 1024 || json[t[a[6]].start] != '[') goto done;
    {
        size_t len = (size_t)stride * height, k = 0; unsigned char *pixels = malloc(len); int x, y;
        if (!pixels) goto done;
        for (v = a[6] + 1; v < t[a[6]].next && v < n; v = t[v].next) {
            int byte;
            if (t[v].parent != a[6] || k >= len || !live_number(json, t, v, &byte) || byte > 255) break;
            pixels[k++] = (unsigned char)byte;
        }
        /* D-Bus image data may omit padding after the final row. */
        if (v == t[a[6]].next && k >= (size_t)stride * (height - 1) + (size_t)width * channels) {
            for (y = 0; y < LIVE_IMAGE_SIDE; y++) for (x = 0; x < LIVE_IMAGE_SIDE; x++) {
                size_t src = (size_t)(y * height / LIVE_IMAGE_SIDE) * stride + (x * width / LIVE_IMAGE_SIDE) * channels;
                size_t dst = (size_t)(y * LIVE_IMAGE_SIDE + x) * 4;
                notice->rgba[dst] = pixels[src]; notice->rgba[dst+1] = pixels[src+1]; notice->rgba[dst+2] = pixels[src+2];
                notice->rgba[dst+3] = channels == 4 ? pixels[src+3] : 255;
            }
            notice->has_image = 1;
        }
        free(pixels);
    }
done:
    free(t); return 1;
fail:
    free(t); return 0;
}

static void live_worker(int fd) {
    int pipefd[2], status; pid_t child; char *line; size_t used = 0; int discard = 0;
    struct pollfd p[2];
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_DFL);
    if (pipe(pipefd) < 0) goto done;
    child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); goto done; }
    if (!child) {
        close(pipefd[0]); close(fd);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        execlp("busctl", "busctl", "--user", "--json=short", "--match=type='method_call',destination='org.freedesktop.Notifications',interface='org.freedesktop.Notifications',member='Notify'", "monitor", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    line = malloc(LIVE_JSON_CAP);
    if (!line) { kill(child, SIGTERM); close(pipefd[0]); waitpid(child, &status, 0); goto done; }
    p[0] = (struct pollfd){.fd = pipefd[0], .events = POLLIN};
    p[1] = (struct pollfd){.fd = fd, .events = POLLIN};
    for (;;) {
        char buf[8192]; ssize_t count, j; int ready = poll(p, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p[1].revents & (POLLHUP | POLLERR | POLLNVAL)) || (p[1].revents & POLLIN)) break;
        if (p[0].revents & (POLLIN | POLLHUP)) {
            count = read(pipefd[0], buf, sizeof buf);
            if (count <= 0) break;
            for (j = 0; j < count; j++) {
                if (buf[j] == '\n') {
                    if (!discard && used) {
                        LiveNotice event; line[used] = 0;
                        if (live_parse(line, &event) && send(fd, &event, sizeof event, MSG_NOSIGNAL) != sizeof event) goto finish;
                    }
                    used = 0; discard = 0;
                } else if (!discard && used + 1 < LIVE_JSON_CAP) line[used++] = buf[j];
                else discard = 1;
            }
        } else if (p[0].revents & (POLLERR | POLLNVAL)) break;
    }
finish:
    free(line); close(pipefd[0]); kill(child, SIGTERM); waitpid(child, &status, 0);
done:
    close(fd); _exit(0);
}
