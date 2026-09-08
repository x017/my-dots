#!/usr/bin/env bash
# monitor-profile.sh — switch which HDMI-A-1 line is active in monitors.conf
# and apply it live via hyprctl, so the change is instant (no reload flicker)
# but also persists across restarts.
#
# Usage:
#   monitor-profile.sh home   # 1920x1080 @ 110Hz
#   monitor-profile.sh work   # 1920x1080 @ 75Hz
#   monitor-profile.sh auto   # preferred
#   monitor-profile.sh --get  # print the currently active profile

set -euo pipefail

CONF="${MONITORS_CONF:-$HOME/.config/hypr/monitors.conf}"

die() { echo "error: $*" >&2; exit 1; }
[ -f "$CONF" ] || die "config not found: $CONF"

apply_live() {
  # -r keeps other settings (workspace binds etc.) untouched, just moves the display
  hyprctl keyword monitor "$1" >/dev/null
}

set_profile() {
  local profile="$1" line=""
  case "$profile" in
    home) line="HDMI-A-1,1920x1080@110Hz,0x0,1" ;;
    work) line="HDMI-A-1,1920x1080@75Hz,0x0,1" ;;
    auto) line="HDMI-A-1,preferred,0x0,1" ;;
    *) die "unknown profile '$profile' (use home|work|auto)" ;;
  esac

  local tmp
  tmp=$(mktemp)

  # Comment every HDMI-A-1 line, then uncomment only the one we want.
  sed -E '/^#?\s*monitor=HDMI-A-1,/ s/^#?\s*/# /' "$CONF" > "$tmp"
  sed -i -E "s|^# monitor=${line}\$|monitor=${line}|" "$tmp"
  sed -i -E "s/^# Active profile:.*/# Active profile: ${profile}/" "$tmp"

  mv "$tmp" "$CONF"

  apply_live "$line"

  # Persist into the live Hyprland config too (belt & braces if reload happens later)
  hyprctl reload >/dev/null 2>&1 || true
}

get_profile() {
  if grep -qE '^monitor=HDMI-A-1,1920x1080@110Hz' "$CONF"; then
    echo home
  elif grep -qE '^monitor=HDMI-A-1,1920x1080@75Hz' "$CONF"; then
    echo work
  elif grep -qE '^monitor=HDMI-A-1,preferred' "$CONF"; then
    echo auto
  else
    echo unknown
  fi
}

case "${1:-}" in
  home|work|auto) set_profile "$1" ;;
  --get)          get_profile ;;
  *) echo "Usage: $(basename "$0") {home|work|auto|--get}" >&2; exit 1 ;;
esac
