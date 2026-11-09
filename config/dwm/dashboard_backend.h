/* Backend shared with the CUI dashboard, used in the native worker process. */
#include <stdbool.h>
#include <ctype.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#define MAX_ITEMS 48
#define MAX_OUTPUT 32768

typedef struct { char ssid[128], mac[32], security[96]; int signal, active; } Network;
typedef struct { char name[160], mac[32]; int paired, connected; } Device;
typedef struct {
    int wifi_available, wifi_enabled, bluetooth_available, bluetooth_enabled, audio_available;
    int muted, volume, networks_count, devices_count, paired_known, connected_known;
    char wifi_device[64];
    Network networks[MAX_ITEMS];
    Device devices[MAX_ITEMS];
    char message[256];
} Snapshot;
typedef enum { ACT_NONE, ACT_WIFI_ON, ACT_WIFI_OFF, ACT_WIFI_SCAN, ACT_WIFI_CONNECT,
               ACT_WIFI_DISCONNECT, ACT_BT_ON, ACT_BT_OFF, ACT_BT_SCAN, ACT_BT_PAIR,
               ACT_BT_CONNECT, ACT_BT_DISCONNECT, ACT_AUDIO_MUTE, ACT_AUDIO_VOLUME } Action;
typedef struct { Action type; char arg[128], password[256]; int value; } Request;
static void wipe(void *buffer, size_t n) {
    volatile unsigned char *p = buffer;
    while (n--) *p++ = 0;
}
static void copystr(char *dst, size_t capacity, const char *src) {
    if (!capacity) return;
    snprintf(dst, capacity, "%s", src ? src : "");
}
#ifndef DASHBOARD_STOPPING
static bool dashboard_stopping(void) { return false; }
#define DASHBOARD_STOPPING() dashboard_stopping()
#endif

/* No shell: names and addresses cannot become commands. Read output with a
 * timeout, and send secrets only through a pipe, never in argv or a file. */
static int run_command(char *const argv[], const char *input, char *out, size_t cap, int timeout_ms) {
    int result = -1, fds[2], in[2], status = 0, flags, done = 0;
    size_t used = 0;
    pid_t pid, waited;
    struct timespec start, now;
    if (cap) out[0] = '\0';
    if (pipe(fds) < 0) return -1;
    if (pipe(in) < 0) { close(fds[0]); close(fds[1]); return -1; }
    pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        dup2(in[0], STDIN_FILENO); dup2(fds[1], STDOUT_FILENO); dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]); close(in[0]); close(in[1]);
        execvp(argv[0], argv); _exit(127);
    }
    close(fds[1]); close(in[0]);
    if (pid < 0) { close(fds[0]); close(in[1]); return -1; }
    if (input && *input) {
        size_t n = strlen(input), written = 0;
        while (written < n) {
            ssize_t w = write(in[1], input + written, n - written);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            written += (size_t)w;
        }
    }
    close(in[1]);
    flags = fcntl(fds[0], F_GETFL, 0); if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!done) {
        char buf[2048]; ssize_t n;
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        poll(&pfd, 1, 100);
        while ((n = read(fds[0], buf, sizeof buf)) > 0) {
            size_t take = cap && used < cap - 1 ? (size_t)n : 0;
            if (take > cap - 1 - used) take = cap - 1 - used;
            if (take) { memcpy(out + used, buf, take); used += take; out[used] = '\0'; }
        }
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) { done = 1; result = WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
        else if (waited < 0 && errno != EINTR) done = 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!done && (DASHBOARD_STOPPING() || (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= timeout_ms)) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
            done = 1;
        }
    }
    /* The child can exit between the last read and waitpid. Drain bytes it
     * wrote before exit so short-lived commands do not lose their output. */
    for (;;) {
        char buf[2048];
        ssize_t n = read(fds[0], buf, sizeof buf);
        size_t take;
        if (n <= 0) break;
        take = cap && used < cap - 1 ? (size_t)n : 0;
        if (take > cap - 1 - used) take = cap - 1 - used;
        if (take) { memcpy(out + used, buf, take); used += take; out[used] = '\0'; }
    }
    close(fds[0]);
    return result;
}

/* nmcli -t escapes colons and backslashes; decode each field in place. */
static int fields(char *line, char **parts, int max) {
    char *src = line, *dst = line;
    int n = 1;
    parts[0] = dst;
    while (*src) {
        if (*src == '\\' && src[1]) { src++; *dst++ = *src++; }
        else if (*src == ':' && n < max) { src++; *dst++ = '\0'; parts[n++] = dst; }
        else *dst++ = *src++;
    }
    *dst = '\0';
    return n;
}

#ifndef DASHBOARD_NO_SNAPSHOT
static int bluetooth_list(char *output, Device *items, int count, int mark) {
    char *line, *save;
    for (line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char mac[32], name[160]; int i;
        if (sscanf(line, "Device %31s %159[^\n]", mac, name) != 2) continue;
        for (i = 0; i < count && strcmp(items[i].mac, mac); i++) ;
        if (mark == 0 && i == count && count < MAX_ITEMS) {
            copystr(items[i].mac, sizeof items[i].mac, mac);
            copystr(items[i].name, sizeof items[i].name, name); count++;
        }
        if (i < count && mark == 1) items[i].paired = 1;
        if (i < count && mark == 2) items[i].connected = 1;
    }
    return count;
}

static void read_snapshot(Snapshot *s) {
    char buf[MAX_OUTPUT], *line, *save, *part[5];
    char *wifi[] = {"nmcli", "-t", "radio", "wifi", NULL};
    char *networks[] = {"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY,BSSID", "device", "wifi", "list", "--rescan", "no", NULL};
    char *ifaces[] = {"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status", NULL};
    char *btshow[] = {"bluetoothctl", "show", NULL};
    char *btdevices[] = {"bluetoothctl", "devices", NULL};
    char *btpaired[] = {"bluetoothctl", "devices", "Paired", NULL};
    char *btconnected[] = {"bluetoothctl", "devices", "Connected", NULL};
    char *vol[] = {"pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL};
    char *mute[] = {"pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL};
    memset(s, 0, sizeof *s);
    if (run_command(wifi, NULL, buf, sizeof buf, 2500) == 0) {
        s->wifi_available = 1; s->wifi_enabled = !strncmp(buf, "enabled", 7);
        if (s->wifi_enabled && run_command(networks, NULL, buf, sizeof buf, 3500) == 0) {
            for (line = strtok_r(buf, "\n", &save); line && s->networks_count < MAX_ITEMS; line = strtok_r(NULL, "\n", &save)) {
                Network *n;
                if (fields(line, part, 5) != 5 || !part[1][0] || !part[4][0]) continue;
                n = &s->networks[s->networks_count++];
                n->active = part[0][0] == '*'; copystr(n->ssid, sizeof n->ssid, part[1]);
                n->signal = atoi(part[2]); copystr(n->security, sizeof n->security, part[3]);
                copystr(n->mac, sizeof n->mac, part[4]);
            }
        }
        if (run_command(ifaces, NULL, buf, sizeof buf, 2500) == 0) {
            for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
                if (fields(line, part, 3) == 3 && !strcmp(part[1], "wifi") && !strncmp(part[2], "connected", 9)) {
                    copystr(s->wifi_device, sizeof s->wifi_device, part[0]); break;
                }
        }
    }
    if (run_command(btshow, NULL, buf, sizeof buf, 2500) == 0 &&
        strstr(buf, "Powered:")) {
        s->bluetooth_available = 1;
        s->bluetooth_enabled = strstr(buf, "Powered: yes") != NULL;
        if (s->bluetooth_enabled && run_command(btdevices, NULL, buf, sizeof buf, 2500) == 0) {
            s->devices_count = bluetooth_list(buf, s->devices, 0, 0);
            if (run_command(btpaired, NULL, buf, sizeof buf, 2500) == 0) { s->paired_known = 1; bluetooth_list(buf, s->devices, s->devices_count, 1); }
            if (run_command(btconnected, NULL, buf, sizeof buf, 2500) == 0) { s->connected_known = 1; bluetooth_list(buf, s->devices, s->devices_count, 2); }
        }
    }
    if (run_command(vol, NULL, buf, sizeof buf, 2500) == 0) {
        char *percent = strchr(buf, '%');
        if (percent) {
            char *start = percent;
            while (start > buf && isdigit((unsigned char)start[-1])) start--;
            if (start != percent) { s->audio_available = 1; s->volume = atoi(start); }
        }
    }
    if (s->audio_available && run_command(mute, NULL, buf, sizeof buf, 2500) == 0) s->muted = strstr(buf, "yes") != NULL;
}

#endif /* DASHBOARD_NO_SNAPSHOT */

/* Last command result is worker-local and never includes Wi-Fi secrets. */
static char dashboard_action_error[256];
static int execute(Request *r) {
    char out[MAX_OUTPUT], secret[320], number[32];
    char *wifi_on[] = {"nmcli", "radio", "wifi", "on", NULL};
    char *wifi_off[] = {"nmcli", "radio", "wifi", "off", NULL};
    char *wifi_scan[] = {"nmcli", "device", "wifi", "rescan", NULL};
    char *wifi_connect[] = {"nmcli", "--wait", "20", "device", "wifi", "connect", r->arg, NULL};
    char *wifi_password[] = {"nmcli", "--wait", "20", "--passwd-file", "/dev/stdin", "device", "wifi", "connect", r->arg, NULL};
    char *wifi_disconnect[] = {"nmcli", "device", "disconnect", r->arg, NULL};
    char *bt_on[] = {"bluetoothctl", "power", "on", NULL};
    char *bt_off[] = {"bluetoothctl", "power", "off", NULL};
    char *bt_scan[] = {"bluetoothctl", "--timeout", "8", "scan", "on", NULL};
    char *bt_pair[] = {"bluetoothctl", "--timeout", "20", "pair", r->arg, NULL};
    char *bt_connect[] = {"bluetoothctl", "--timeout", "20", "connect", r->arg, NULL};
    char *bt_disconnect[] = {"bluetoothctl", "--timeout", "10", "disconnect", r->arg, NULL};
    char *audio_mute[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL};
    char *audio_volume[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", number, NULL};
    char **command = NULL; int timeout = 5000, rc;
    secret[0] = '\0'; dashboard_action_error[0] = '\0';
    switch (r->type) {
    case ACT_WIFI_ON: command = wifi_on; break; case ACT_WIFI_OFF: command = wifi_off; break;
    case ACT_WIFI_SCAN: command = wifi_scan; timeout = 12000; break;
    case ACT_WIFI_CONNECT:
        timeout = 25000;
        if (r->password[0]) { command = wifi_password; snprintf(secret, sizeof secret, "802-11-wireless-security.psk:%s\n", r->password); }
        else command = wifi_connect;
        break;
    case ACT_WIFI_DISCONNECT: command = wifi_disconnect; timeout = 12000; break;
    case ACT_BT_ON: command = bt_on; break; case ACT_BT_OFF: command = bt_off; break;
    case ACT_BT_SCAN: command = bt_scan; timeout = 12000; break;
    case ACT_BT_PAIR: command = bt_pair; timeout = 25000; break;
    case ACT_BT_CONNECT: command = bt_connect; timeout = 25000; break;
    case ACT_BT_DISCONNECT: command = bt_disconnect; timeout = 15000; break;
    case ACT_AUDIO_MUTE: command = audio_mute; break;
    case ACT_AUDIO_VOLUME: snprintf(number, sizeof number, "%d%%", r->value); command = audio_volume; break;
    default: return -1;
    }
    rc = run_command(command, secret[0] ? secret : NULL, out, sizeof out, timeout);
    /* bluetoothctl often exits zero even for a command-level failure. */
    if (command[0] && !strcmp(command[0], "bluetoothctl")) {
        if (strstr(out, "Failed") || strstr(out, "not available") || strstr(out, "No default controller") ||
            strstr(out, "NotAuthorized") || strstr(out, "Not authorized") || strstr(out, "Permission denied") ||
            strstr(out, "not permitted") || strstr(out, "AuthenticationFailed")) rc = 1;
        else if (r->type == ACT_BT_SCAN && strstr(out, "Discovery started")) rc = 0;
        if (rc) {
            if (strstr(out, "NotAuthorized") || strstr(out, "Not authorized") || strstr(out, "Permission denied") || strstr(out, "not permitted"))
                copystr(dashboard_action_error, sizeof dashboard_action_error, "Bluetooth authorization denied. Check your active login session and polkit agent.");
            else if (strstr(out, "Authentication") || r->type == ACT_BT_PAIR)
                copystr(dashboard_action_error, sizeof dashboard_action_error, "Bluetooth pairing needs an active pairing agent or device confirmation.");
            else copystr(dashboard_action_error, sizeof dashboard_action_error, "Bluetooth action failed. Check Bluetooth service and device.");
        }
    }
    wipe(secret, sizeof secret); wipe(out, sizeof out);
    return rc;
}

