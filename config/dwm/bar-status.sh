#!/bin/sh
# Native dwm bar provider. The parent PID is supplied by dwm; do not run a
# second status writer in .xinitrc. All potentially slow commands run here,
# never in dwm's event loop.
export LC_ALL=C
parent=${1:-$PPID}
old_total=0
old_idle=0
while kill -0 "$parent" 2>/dev/null; do
    cpu='--%'
    if [ -r /proc/stat ]; then
        # Linux's first four counters plus iowait, irq, softirq and steal.
        read -r label user nice system idle iowait irq softirq steal rest < /proc/stat
        total=$((user + nice + system + idle + iowait + irq + softirq + steal))
        idle_total=$((idle + iowait))
        delta=$((total - old_total))
        idle_delta=$((idle_total - old_idle))
        if [ "$old_total" -gt 0 ] && [ "$delta" -gt 0 ] && [ "$idle_delta" -ge 0 ] && [ "$idle_delta" -le "$delta" ]; then
            cpu="$((100 * (delta - idle_delta) / delta))%"
        fi
        old_total=$total
        old_idle=$idle_total
    fi

    audio='Audio --'
    if command -v pactl >/dev/null 2>&1; then
        mute=$(pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null)
        if [ "$mute" = 'Mute: yes' ]; then
            audio='Muted'
        elif [ "$mute" = 'Mute: no' ]; then
            volume=$(pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | sed -n 's/.* \([0-9][0-9]*%\).*/\1/p' | head -n 1)
            [ -n "$volume" ] && audio=$volume
        fi
    fi

    network='Offline'
    if command -v nmcli >/dev/null 2>&1; then
        state=$(nmcli -t -f TYPE,STATE device status 2>/dev/null)
        case "$state" in
            *wifi:connected*) network='Wi-Fi' ;;
            *ethernet:connected*) network='Ethernet' ;;
            *:connected*) network='Connected' ;;
        esac
    else
        for iface in /sys/class/net/*; do
            [ "${iface##*/}" = lo ] && continue
            [ -r "$iface/operstate" ] || continue
            read -r state < "$iface/operstate"
            if [ "$state" = up ]; then network='Connected'; break; fi
        done
    fi
    clock=$(date '+%a %d %b  %H:%M')
    xsetroot -name "DWM_BAR_V1|$cpu|$audio|$network|$clock" 2>/dev/null || exit
    sleep 2
done
