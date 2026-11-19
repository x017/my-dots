# mel

mel is an X11 screen locker with PAM authentication, multi-monitor support and
Cairo-rendered layouts. This directory contains source copied from
[x017/mel](https://github.com/x017/mel), revision
`06487b675b9f56432a247ddaaad11a1017e46638`, with the additions below. It is
not a submodule and does not contain the upstream Git metadata or object files.
The upstream repository currently specifies no license.

## Build and install

Install a C compiler, `make`, `pkg-config`, and the X11, XRandR, Cairo and PAM
development packages. On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libx11-dev libxrandr-dev libcairo2-dev libpam0g-dev
cd config/mel
make
sudo make install
```

The default install prefix is `/usr/local`; override with `make PREFIX=...` and
use the same value for install. Installation places the `mel` binary, manual,
example config and themes under the prefix. It does **not** overwrite your
personal config. The default PAM service is `login`; check that the chosen PAM
service works on your system before relying on the locker.

From dwm use Alt+Shift+L or the bar's lock button; both launch `mel`. To keep
English accessible when locking from Persian, use `dwm --lock` instead of
calling `mel` directly for external idle-lock triggers (see `../dwm/README`).

## Layouts and colours

Layouts are `minimal`, `card`, `left`, `pill`, `avatar`, `band`, `terminal`,
`corner`, `ring`, `reactor`, and the new **split**, **zen** and **horizon**.
Split puts the clock and sign-in area on opposite sides and stacks them on
small screens; zen is centered with plenty of space; horizon puts the clock in
the top corner and sign-in near the bottom. They all retain mel's standard
password, failure, caps-lock and unlock feedback.

The bundled colour themes are `nord`, `catppuccin`, `tokyo-night`, and the new
`my-dots` (matching the bar), `rose-pine`, `gruvbox` and `everforest`. Layouts
and themes are independent:

```sh
mel --list-layouts
mel --layout split --theme my-dots
```

For a persistent choice, copy `config.example` to
`${XDG_CONFIG_HOME:-$HOME/.config}/mel/config` and set, for example:

```ini
layout = zen
theme = rose-pine
```

Themes load before the rest of your config, so colour settings in the config
override theme colours. User themes are read from `$XDG_CONFIG_HOME/mel/themes`
then `~/.config/mel/themes`; installed themes are the fallback. When running
from the source directory, the local `themes` directory is also a fallback.

## Preview without locking

```sh
mel --layout horizon --theme my-dots --preview /tmp/mel-preview.png
mel --layout split --theme gruvbox --preview /tmp/mel-small.png --preview-size 800x600
```

Preview writes a PNG and exits: it does not open the X display, grab input or
run PAM. `solid`, `gradient` and PNG image backgrounds are shown; `blur` and
`dim` use the theme's gradient instead, since preview does not capture your
actual desktop. The preview size defaults to 1280x720 and can range from
320x240 to 4096x2160. Run `make check` for headless layout and render tests.

`make check` tests rendering and configuration only; it does not verify real
X11 grabs, monitor changes, keyboard input or PAM authentication. After
installing, test locking and unlocking in your own X session with a TTY or SSH
fallback before using it for unattended locking. The locker code is inherited
from upstream and has not had a security audit here.
