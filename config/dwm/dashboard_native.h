/* In-process Xlib/Drw control center. A short-lived fork does all blocking
 * command work; no Xlib calls occur in the worker. */
typedef struct { Snapshot snapshot; unsigned int completed; } DashboardFrame;
typedef struct {
  char label[320];
  Action action;
  int item;
} DashboardRow;

static int
dashboardwrite(int fd, const void *data, size_t size)
{
  const char *p = data;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 0;
    p += n; size -= (size_t)n;
  }
  return 1;
}

static void
dashboardworker(int fd)
{
  DashboardFrame frame = {0};
  Request req;
  struct pollfd pollfd = { .fd = fd, .events = POLLIN };
  char message[256] = "";
  signal(SIGCHLD, SIG_DFL);
  signal(SIGPIPE, SIG_IGN);
  for (;;) {
    int n;
    read_snapshot(&frame.snapshot);
    copystr(frame.snapshot.message, sizeof frame.snapshot.message, message);
    if (!dashboardwrite(fd, &frame, sizeof frame)) break;
    n = poll(&pollfd, 1, 3000);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 || (pollfd.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
    if (n > 0 && (pollfd.revents & POLLIN)) {
      size_t used = 0;
      while (used < sizeof req) {
        ssize_t got = read(fd, (char *)&req + used, sizeof req - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto done;
        used += (size_t)got;
      }
      n = execute(&req);
      if (!n) copystr(message, sizeof message, "Done.");
      else if (req.type == ACT_BT_PAIR) copystr(message, sizeof message, "Pairing failed. A Bluetooth pairing agent may be required.");
      else if (req.type == ACT_WIFI_CONNECT) copystr(message, sizeof message, "Connection failed. Check password or NetworkManager.");
      else copystr(message, sizeof message, "Action failed. Check device, service and permissions.");
      wipe(&req, sizeof req);
      frame.completed++;
    }
  }
done:
  close(fd);
  _exit(0);
}

static void
dashboardrow(DashboardRow *rows, int *count, Action action, int item, const char *fmt, ...)
{
  va_list ap;
  if (*count >= 120) return;
  rows[*count].action = action; rows[*count].item = item;
  va_start(ap, fmt); vsnprintf(rows[*count].label, sizeof rows[*count].label, fmt, ap); va_end(ap);
  (*count)++;
}

static void
dashboardnative(const Arg *arg)
{
  Monitor *m = selmon;
  XSetWindowAttributes wa;
  XEvent ev;
  Window win;
  Drw *d;
  Clr *normal, *selected, *head;
  const char *font[] = {"monospace:size=10", "Symbols Nerd Font:size=11"};
  const char *nc[] = {"#adbac7", "#121417", "#1e2025"};
  const char *sc[] = {"#f2d49e", "#181a1e", "#f2d49e"};
  const char *hc[] = {"#8a949e", "#090b0d", "#2d3035"};
  Snapshot snapshot = {0};
  DashboardFrame incoming;
  unsigned int completed = 0, waiting_for = 0;
  DashboardRow rows[120];
  Request req;
  char password[256] = "", wifi_mac[32] = "", status[256] = "Loading devices...";
  size_t received = 0;
  int fd[2], width, height, rh, visible, choice = 0, start, count, i, done = 0;
  int busy = 1, page = 0, password_mode = 0, selected_bt = -1;
  char bt_mac[32] = "";
  pid_t pid;
  (void)arg;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) return;
  fcntl(fd[0], F_SETFD, FD_CLOEXEC);
  fcntl(fd[1], F_SETFD, FD_CLOEXEC);
  pid = fork();
  if (pid < 0) { close(fd[0]); close(fd[1]); return; }
  if (!pid) { close(fd[0]); close(ConnectionNumber(dpy)); dashboardworker(fd[1]); }
  close(fd[1]);
  fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
  if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) { close(fd[0]); return; }
  width = MAX(1, MIN(700, m->mw - 20)); rh = MAX(30, bh + 6);
  visible = MAX(1, MIN(12, (m->mh - 5 * rh - 20) / rh)); height = (visible + 5) * rh;
  wa.override_redirect = True; wa.background_pixel = BlackPixel(dpy, screen);
  wa.event_mask = ExposureMask | ButtonPressMask;
  win = XCreateWindow(dpy, root, m->mx + (m->mw - width) / 2, m->my + (m->mh - height) / 2,
                      width, height, 1, DefaultDepth(dpy, screen), CopyFromParent,
                      DefaultVisual(dpy, screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
  d = drw_create(dpy, screen, root, width, height);
  drw_fontset_create(d, font, LENGTH(font));
  normal = drw_scm_create(d, nc, 3); selected = drw_scm_create(d, sc, 3); head = drw_scm_create(d, hc, 3);
  XSetWindowBorder(dpy, win, head[ColBorder].pixel); XMapRaised(dpy, win);
  while (!done && running) {
    fd_set readfds;
    struct timeval tv = { 0, 250000 };
    int xfd = ConnectionNumber(dpy), ready;
    count = 0;
    if (page == 0) {
      if (!snapshot.wifi_available) dashboardrow(rows, &count, ACT_NONE, 0, "NetworkManager / nmcli unavailable");
      else {
        dashboardrow(rows, &count, snapshot.wifi_enabled ? ACT_WIFI_OFF : ACT_WIFI_ON, 0, "Wi-Fi: %s  [toggle]", snapshot.wifi_enabled ? "ON" : "OFF");
        if (snapshot.wifi_enabled) {
          dashboardrow(rows, &count, ACT_WIFI_SCAN, 0, "Scan for networks");
          if (snapshot.wifi_device[0]) dashboardrow(rows, &count, ACT_WIFI_DISCONNECT, 0, "Disconnect current network");
          for (i = 0; i < snapshot.networks_count; i++) {
            Network *n = &snapshot.networks[i];
            dashboardrow(rows, &count, ACT_WIFI_CONNECT, i, "%s  /  %d%%  /  %s%s", n->ssid, n->signal,
                         n->security[0] && strcmp(n->security, "--") ? n->security : "Open", n->active ? "  /  CONNECTED" : "");
          }
        }
      }
    } else if (page == 1) {
      if (!snapshot.bluetooth_available) dashboardrow(rows, &count, ACT_NONE, 0, "BlueZ controller / bluetoothctl unavailable");
      else {
        dashboardrow(rows, &count, snapshot.bluetooth_enabled ? ACT_BT_OFF : ACT_BT_ON, 0, "Bluetooth: %s  [toggle]", snapshot.bluetooth_enabled ? "ON" : "OFF");
        if (snapshot.bluetooth_enabled) {
          dashboardrow(rows, &count, ACT_BT_SCAN, 0, "Scan for devices (8 seconds)");
          for (i = 0; i < snapshot.devices_count; i++) {
            Device *dev = &snapshot.devices[i];
            if (!strcmp(bt_mac, dev->mac)) selected_bt = i;
            dashboardrow(rows, &count, ACT_NONE, i + 1, "%s  /  %s%s", dev->name, dev->mac,
                         dev->connected ? "  /  CONNECTED" : dev->paired ? "  /  PAIRED" : "");
          }
          if (bt_mac[0] && selected_bt >= 0 && selected_bt < snapshot.devices_count && !strcmp(snapshot.devices[selected_bt].mac, bt_mac)) {
            Device *dev = &snapshot.devices[selected_bt];
            dashboardrow(rows, &count, ACT_BT_PAIR, selected_bt, "Pair with %s", dev->name);
            dashboardrow(rows, &count, ACT_BT_CONNECT, selected_bt, "Connect to %s", dev->name);
            dashboardrow(rows, &count, ACT_BT_DISCONNECT, selected_bt, "Disconnect %s", dev->name);
          }
        }
      }
    } else {
      if (!snapshot.audio_available) dashboardrow(rows, &count, ACT_NONE, 0, "Audio server / pactl unavailable");
      else {
        dashboardrow(rows, &count, ACT_AUDIO_MUTE, 0, "Default output: %d%%  /  %s  [toggle mute]", snapshot.volume, snapshot.muted ? "MUTED" : "ON");
        dashboardrow(rows, &count, ACT_AUDIO_VOLUME, MAX(0, snapshot.volume - 5), "Volume down 5%%");
        dashboardrow(rows, &count, ACT_AUDIO_VOLUME, MIN(100, snapshot.volume + 5), "Volume up 5%%");
      }
    }
    if (!count) dashboardrow(rows, &count, ACT_NONE, 0, "No items available");
    choice = MAX(0, MIN(choice, count - 1)); start = choice >= visible ? choice - visible + 1 : 0;
    drw_setscheme(d, head); drw_rect(d, 0, 0, width, height, 1, 1);
    drw_text(d, 12, 4, width - 24, rh, 8, "C O N T R O L   C E N T E R", 0);
    drw_text(d, 12, rh, width - 24, rh, 8, page == 0 ? "[ WI-FI ]    BLUETOOTH    SOUND" : page == 1 ? "  WI-FI    [ BLUETOOTH ]    SOUND" : "  WI-FI      BLUETOOTH    [ SOUND ]", 0);
    for (i = 0; i < visible; i++) {
      int index = start + i;
      drw_setscheme(d, index == choice ? selected : normal);
      drw_text(d, 12, 2 * rh + i * rh, width - 24, rh - 2, 10, index < count ? rows[index].label : "", 0);
    }
    drw_setscheme(d, head);
    if (password_mode) {
      char masked[300]; size_t len = strlen(password);
      snprintf(masked, sizeof masked, "Password: %.*s  (Enter to connect; empty = saved)", (int)len, "***************************************************************************************************************************************************************************************************************************************************************");
      drw_text(d, 12, (visible + 2) * rh, width - 24, rh, 8, masked, 0);
    } else drw_text(d, 12, (visible + 2) * rh, width - 24, rh, 8, busy ? "Working..." : status, 0);
    drw_text(d, 12, (visible + 3) * rh, width - 24, rh, 8, "LEFT/RIGHT tab   UP/DOWN select   ENTER action   ESC close", 0);
    if (page == 1) drw_text(d, 12, (visible + 4) * rh, width - 24, rh, 8, "Pairing confirmation may require a separate Bluetooth agent.", 0);
    drw_map(d, win, 0, 0, width, height);
    FD_ZERO(&readfds); FD_SET(xfd, &readfds); FD_SET(fd[0], &readfds);
    ready = XPending(dpy) ? 1 : select(MAX(xfd, fd[0]) + 1, &readfds, NULL, NULL, &tv);
    if (ready < 0 && errno != EINTR) break;
    if (ready > 0 && FD_ISSET(fd[0], &readfds)) {
      ssize_t n = read(fd[0], (char *)&incoming + received, sizeof incoming - received);
      if (n > 0) {
        received += (size_t)n;
        if (received == sizeof incoming) { snapshot = incoming.snapshot; completed = incoming.completed; received = 0;
          if (completed >= waiting_for) busy = 0;
          if (!busy) copystr(status, sizeof status, snapshot.message); }
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { copystr(status, sizeof status, "Backend stopped."); busy = 1; done = 1; }
    }
    while (XPending(dpy)) {
      KeySym key = NoSymbol; char text[64]; int len = 0, activate = 0;
      XNextEvent(dpy, &ev);
      if (ev.type == KeyPress) {
        len = XLookupString(&ev.xkey, text, sizeof text, &key, NULL);
        if (password_mode) {
          size_t used = strlen(password);
          if (key == XK_Escape) { password_mode = 0; wipe(password, sizeof password); }
          else if (key == XK_BackSpace && used) password[used - 1] = 0;
          else if (key == XK_Return || key == XK_KP_Enter) {
            memset(&req, 0, sizeof req); req.type = ACT_WIFI_CONNECT;
            copystr(req.arg, sizeof req.arg, wifi_mac); copystr(req.password, sizeof req.password, password);
            if (send(fd[0], &req, sizeof req, MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof req) { busy = 1; waiting_for = completed + 1; copystr(status, sizeof status, "Connecting..."); }
            else copystr(status, sizeof status, "Could not send request to backend.");
            wipe(&req, sizeof req); wipe(password, sizeof password); password_mode = 0;
          } else if (len > 0 && used + (size_t)len < sizeof password && !(ev.xkey.state & ControlMask)) {
            int valid = 1; for (i = 0; i < len; i++) if ((unsigned char)text[i] < 32 || text[i] == 127) valid = 0;
            if (valid) { memcpy(password + used, text, (size_t)len); password[used + len] = 0; }
          }
          wipe(text, sizeof text);
          continue;
        }
        if (key == XK_Escape) done = 1;
        else if (key == XK_Left || key == XK_Right || key == XK_Tab) { page = (page + (key == XK_Left ? 2 : 1)) % 3; choice = 0; }
        else if (key == XK_Down || key == XK_j) choice = (choice + 1) % count;
        else if (key == XK_Up || key == XK_k) choice = (choice + count - 1) % count;
        else if (key == XK_Return || key == XK_KP_Enter) activate = 1;
      } else if (ev.type == ButtonPress && ev.xbutton.window == win && !password_mode) {
        if (ev.xbutton.button == Button4) choice = (choice + count - 1) % count;
        else if (ev.xbutton.button == Button5) choice = (choice + 1) % count;
        else if (ev.xbutton.button == Button1 && ev.xbutton.y >= 2 * rh && ev.xbutton.y < (2 + visible) * rh) {
          i = start + (ev.xbutton.y - 2 * rh) / rh; if (i < count) { choice = i; activate = 1; }
        }
      } else if (ev.type == ConfigureNotify && ev.xconfigure.window == root) { done = 1; configurenotify(&ev); }
      else if (ev.type < LASTEvent && ev.type != Expose && ev.type != EnterNotify && ev.type != FocusIn && ev.type != KeyRelease && handler[ev.type]) handler[ev.type](&ev);
      if (activate && !busy) {
        DashboardRow *row = &rows[choice];
        memset(&req, 0, sizeof req); req.type = row->action;
        if (page == 1 && row->action == ACT_NONE && row->item > 0) {
          selected_bt = row->item - 1; copystr(bt_mac, sizeof bt_mac, snapshot.devices[selected_bt].mac);
          copystr(status, sizeof status, "Device selected: choose pair, connect or disconnect below.");
        } else if (row->action == ACT_WIFI_CONNECT) {
          Network *n = &snapshot.networks[row->item];
          if (!n->active) {
            copystr(wifi_mac, sizeof wifi_mac, n->mac);
            if (n->security[0] && strcmp(n->security, "--")) { password_mode = 1; req.type = ACT_NONE; }
            else copystr(req.arg, sizeof req.arg, n->mac);
          } else req.type = ACT_NONE;
        } else if (row->action == ACT_WIFI_DISCONNECT) copystr(req.arg, sizeof req.arg, snapshot.wifi_device);
        else if (row->action == ACT_BT_PAIR || row->action == ACT_BT_CONNECT || row->action == ACT_BT_DISCONNECT)
          copystr(req.arg, sizeof req.arg, snapshot.devices[row->item].mac);
        else if (row->action == ACT_AUDIO_VOLUME) req.value = row->item;
        if (req.type != ACT_NONE) {
          if (send(fd[0], &req, sizeof req, MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof req) { busy = 1; waiting_for = completed + 1; }
          else copystr(status, sizeof status, "Could not send request to backend.");
        }
        wipe(&req, sizeof req);
      }
    }
  }
  close(fd[0]); wipe(password, sizeof password); wipe(&req, sizeof req);
  XDestroyWindow(dpy, win); XUngrabKeyboard(dpy, CurrentTime);
  drw_scm_free(d, normal, 3); drw_scm_free(d, selected, 3); drw_scm_free(d, head, 3); drw_free(d);
}
