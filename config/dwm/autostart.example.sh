#!/bin/sh
# Example for ~/.local/share/dwm/autostart.sh. Merge with your existing file;
# do not run alongside the old hard-coded feh/setxkbmap/refreshmenu lines.
config=${XDG_CONFIG_HOME:-$HOME/.config}

nm-applet &
xrandr --output eDP-1 --right-of HDMI-1
"$config/dwm/refreshmenu.sh" --apply

# Restore after xrandr, so the saved wallpaper matches the final geometry.
if [ -x "$HOME/.fehbg" ]; then
    "$HOME/.fehbg" &
elif [ -f "$HOME/Downloads/wallpaper/glassmorphsesimjapon.png" ]; then
    feh --bg-fill "$HOME/Downloads/wallpaper/glassmorphsesimjapon.png" &
fi

# dwm itself handles Win+Space. Do not also set grp:win_space_toggle.
setxkbmap -layout us,ir -option ''
