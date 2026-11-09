#!/bin/sh
# Example for ~/.local/share/dwm/autostart.sh. Merge with your existing file;
# do not run alongside the old hard-coded feh/setxkbmap/refreshmenu lines.
nm-applet &
xrandr --output eDP-1 --right-of HDMI-1
dwm --apply-display-settings

# Restore after xrandr, so the saved wallpaper matches the final geometry.
if [ -x "$HOME/.fehbg" ]; then
    "$HOME/.fehbg" &
elif [ -f "$HOME/Downloads/wallpaper/glassmorphsesimjapon.png" ]; then
    feh --bg-fill "$HOME/Downloads/wallpaper/glassmorphsesimjapon.png" &
fi

# dwm uses the existing XKB Win+Space option when configured; otherwise
# it switches groups itself. Other keyboard options are left unchanged.
setxkbmap -layout us,ir
