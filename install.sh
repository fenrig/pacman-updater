#!/bin/sh
# Deliberately does not source the application configuration.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'run as root' >&2; exit 1; }
build=${1:-build}
config=${2:-/etc/pacman-updater.conf}
schedule='*-*-* 00:00:00'
if [ -r "$config" ]; then
    schedule=$(awk -F= '$1 == "timer_calendar" { sub(/^[[:space:]]*/,"",$2); sub(/[[:space:]]*$/,"",$2); print $2; exit }' "$config")
    [ -n "$schedule" ] || schedule='*-*-* 00:00:00'
fi
install -Dm755 "$build/pacman-updater" /usr/bin/pacman-updater
install -Dm755 "$build/pacman-updater-check" /usr/bin/pacman-updater-check
install -Dm644 completions/pacman-updater.bash \
    /usr/share/bash-completion/completions/pacman-updater
install -Dm644 config.example /etc/pacman-updater.conf.new
install -Dm644 systemd/pacman-updater.service /etc/systemd/system/pacman-updater.service
sed "s|^OnCalendar=.*|OnCalendar=$schedule|" systemd/pacman-updater.timer > /etc/systemd/system/pacman-updater.timer
install -Dm755 tools/pacman-updater-pacman-wrapper /usr/local/libexec/pacman-updater-pacman-wrapper
install -d -m700 /var/lib/pacman-updater
echo 'Review /etc/pacman-updater.conf.new, privilege policy, and timer before enabling.'
