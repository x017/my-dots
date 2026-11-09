#!/bin/sh
# Choose a display mode with rofi, or restore saved modes with --apply.
# This preserves the user's existing ~/.config/refreshrate.conf format.
export LC_ALL=C
conf="${XDG_CONFIG_HOME:-$HOME/.config}/refreshrate.conf"

if [ "${1:-}" = --apply ]; then
    [ -r "$conf" ] || exit 0
    while read -r output resolution rate rest; do
        [ -n "$output" ] && [ -n "$resolution" ] && [ -n "$rate" ] || continue
        xrandr --output "$output" --mode "$resolution" --rate "$rate" || \
            printf 'refreshmenu: could not restore %s %s %s\n' "$output" "$resolution" "$rate" >&2
    done < "$conf"
    exit 0
fi

# Apply and persist one monitor without a shell-evaluated command line.
save_mode() {
    output=$1 resolution=$2 rate=$3
    [ -n "$output" ] && [ -n "$resolution" ] && [ -n "$rate" ] || return 1
    xrandr --output "$output" --mode "$resolution" --rate "$rate" || return 1
    mkdir -p "$(dirname "$conf")" || return 1
    tmp=$(mktemp "${conf}.XXXXXX") || return 1
    trap 'rm -f "$tmp"' EXIT HUP INT TERM
    if [ -f "$conf" ]; then awk -v out="$output" '$1 != out' "$conf" > "$tmp" || return 1; fi
    printf '%s %s %s\n' "$output" "$resolution" "$rate" >> "$tmp" || return 1
    mv -f "$tmp" "$conf"
}

if [ "${1:-}" = --set ]; then
    [ "$#" -eq 4 ] || exit 2
    save_mode "$2" "$3" "$4"
    exit $?
fi

menu() {
    count=$2
    [ "$count" -gt 10 ] && count=10
    rofi -dmenu -i -p "$1" \
      -theme-str 'window { width: 380px; background-color: #090b0d; border: 1px; border-color: #2d3035; } listview { lines: 10; } element { text-color: #adbac7; background-color: #121417; } element selected { text-color: #f2d49e; background-color: #181a1e; }' \
      -theme-str "listview { lines: $count; }"
}

outputs=$(xrandr --query | awk '/ connected/{print $1}')
[ -n "$outputs" ] || { echo 'refreshmenu: no connected displays' >&2; exit 1; }
count=$(printf '%s\n' "$outputs" | wc -l)
if [ "$count" -gt 1 ]; then
    output=$(printf '%s\n' "$outputs" | menu 'Monitor' "$count") || exit 0
else
    output=$outputs
fi
[ -n "$output" ] || exit 0
# Only accept a connected output returned by xrandr, never arbitrary menu text.
printf '%s\n' "$outputs" | grep -Fx -- "$output" >/dev/null || exit 1

choices=$(xrandr --query | awk -v out="$output" '
    $1==out {f=1; next}
    /^[^ ]/ {f=0}
    f && $1 ~ /^[0-9]+x[0-9]+$/ {
        split($1, d, "x")
        for (i=2; i<=NF; i++) {
            r=$i; gsub(/[*+]/, "", r)
            if (r ~ /^[0-9]+([.][0-9]+)?$/) print d[1], d[2], r, $1
        }
    }' | sort -k1,1nr -k2,2nr -k3,3nr | awk '{printf "%s  %s Hz\n", $4, $3}')
[ -n "$choices" ] || exit 1
count=$(printf '%s\n' "$choices" | wc -l)
pick=$(printf '%s\n' "$choices" | menu "$output" "$count") || exit 0
[ -n "$pick" ] || exit 0
printf '%s\n' "$choices" | grep -Fx -- "$pick" >/dev/null || exit 1
resolution=$(printf '%s\n' "$pick" | awk '{print $1}')
rate=$(printf '%s\n' "$pick" | awk '{print $2}')
save_mode "$output" "$resolution" "$rate"
