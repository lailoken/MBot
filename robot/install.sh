#!/usr/bin/env bash
# Install or update MBot's robot-side pieces on a rooted Vector (docs/ROBOT_MODIFICATIONS.md §6-§8):
#   mictap (mic streamer + direction-of-sound server), the mic shim in vic-anim, and the shim safety net.
#
#   robot/install.sh <robot-ip>            install, or update what changed (asks before rebooting him)
#   robot/install.sh <robot-ip> --yes      don't ask
#   robot/install.sh <robot-ip> --check    only show what's installed
#
# Needs root SSH to the robot: $VSSH (default ~/.config/mbot/vssh), used as `vssh IP 'command'`, with
# stdin passed through (see docs §3). Every step is reversible; see the "Undo" blocks in the docs.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
IP=${1:-}
[ -n "$IP" ] || { sed -n '2,10p' "$0"; exit 1; }
shift
YES=0; CHECK=0
for a in "$@"; do case "$a" in --yes|-y) YES=1;; --check) CHECK=1;; *) echo "unknown option $a"; exit 1;; esac; done
VSSH=${VSSH:-$HOME/.config/mbot/vssh}
[ -x "$VSSH" ] || { echo "no SSH helper at $VSSH - set up root SSH first (docs/ROBOT_MODIFICATIONS.md §3)"; exit 1; }
r() { "$VSSH" "$IP" "$@" 2>/dev/null; }

r 'true' || { echo "can't reach the robot at $IP over SSH"; exit 1; }
status() {
  r 'echo "mictap:        $(systemctl is-active mictap 2>/dev/null) ($( [ -x /data/mictap ] && md5sum /data/mictap | cut -c1-8 || echo none))";
     echo "mic shim:      $( [ -f /etc/systemd/system/vic-anim.service.d/mbot-mic.conf ] && echo installed || echo not installed), loaded: $(grep -c libmbotmic /proc/$(pidof vic-anim)/maps 2>/dev/null || echo 0)";
     echo "safety net:    $( [ -e /etc/systemd/system/timers.target.wants/mbot-shim-guard.timer ] && echo on || echo off)";
     echo "firmware hears: $( [ -e /data/mbot/mic_silence ] && echo no || echo yes)"'
}
if [ "$CHECK" = 1 ]; then status; exit 0; fi

[ -f "$HERE/out/mictap" ] && [ -f "$HERE/out/libmbotmic.so" ] || { echo "== building the robot pieces"; "$HERE/build.sh"; }

# upload <local> <remote>: only if it differs; verified by checksum; the old one kept as .prev
upload() {
  local l=$1 rp=$2 lm rm
  lm=$(md5sum "$l" | cut -d' ' -f1)
  rm=$(r "md5sum $rp 2>/dev/null | cut -d' ' -f1" || true)
  [ "$lm" = "$rm" ] && { echo "   $rp is up to date"; return 1; }
  "$VSSH" "$IP" "mkdir -p $(dirname "$rp") && cat > $rp.new" < "$l" 2>/dev/null
  [ "$(r "md5sum $rp.new | cut -d' ' -f1")" = "$lm" ] || { echo "upload of $rp failed (checksum)"; exit 1; }
  r "chmod 755 $rp.new; [ -f $rp ] && cp -f $rp $rp.prev; mv -f $rp.new $rp"
  echo "   $rp updated"
  return 0
}
# put <remote> <content>: write a small text file (base64 avoids quoting trouble)
put() { r "echo $(printf '%s' "$2" | base64 -w0) | base64 -d > $1"; }

need_reboot=0
echo "== mictap"
if upload "$HERE/out/mictap" /data/mictap; then r 'systemctl restart mictap 2>/dev/null || true'; fi

echo "== mic shim"
r 'mkdir -p /data/mbot'
if upload "$HERE/out/libmbotmic.so" /data/mbot/libmbotmic.so.candidate; then :; fi
upload "$HERE/out/shimtest" /data/mbot/shimtest >/dev/null || true
if ! r 'cd /data/mbot && LD_PRELOAD=./libmbotmic.so.candidate /bin/true && LD_PRELOAD=./libmbotmic.so.candidate ./shimtest | grep -q fwd_ok=1'; then
  echo "   the shim failed its self-test on the robot - not installing it"; r 'systemctl restart mictap 2>/dev/null || true'; exit 1
fi
# The self-test borrows mictap's raw-mic socket (and removes it afterwards): give mictap a fresh one.
r 'systemctl restart mictap 2>/dev/null || true'

if [ "$(r 'md5sum /data/mbot/libmbotmic.so.candidate | cut -d" " -f1')" != "$(r 'md5sum /data/mbot/libmbotmic.so 2>/dev/null | cut -d" " -f1')" ]; then
  r 'cd /data/mbot; [ -f libmbotmic.so ] && cp -f libmbotmic.so libmbotmic.so.prev; cp -f libmbotmic.so.candidate libmbotmic.so'
  echo "   shim updated (loads at the next reboot)"; need_reboot=1
fi

echo "== services"
GUARD='#!/bin/sh
# MBot mic-shim safety net (run by mbot-shim-guard.timer ~150 s after boot). If vic-anim is not running,
# or a fault is pending, while the LD_PRELOAD mic shim is installed: remove the shim and reboot to stock.
# NOTE: /run/fault_code is a FIFO - it always exists; never read it. A real fault is /run/fault_code.pending.
DROPIN=/etc/systemd/system/vic-anim.service.d/mbot-mic.conf
LOG=/data/mbot/shim-guard.log
[ -f "$DROPIN" ] || exit 0
if pidof vic-anim >/dev/null && [ ! -e /run/fault_code.pending ]; then
  echo "$(date) boot OK: vic-anim healthy with mic shim" >> $LOG; exit 0
fi
echo "$(date) vic-anim NOT healthy with mic shim - removing shim, rebooting" >> $LOG
mount -o remount,rw / && rm -f "$DROPIN" && sync && mount -o remount,ro /
/sbin/reboot
'
put /data/mbot/shim-guard.sh "$GUARD"; r 'chmod 755 /data/mbot/shim-guard.sh'
if ! r '[ -f /etc/systemd/system/mictap.service ] && [ -f /etc/systemd/system/vic-anim.service.d/mbot-mic.conf ] && [ -f /etc/systemd/system/mbot-shim-guard.timer ]'; then
  MICTAP='[Unit]
Description=MBot microphone tap (streams Vector mic to the PC)
After=vic-cloud.service vic-robot.service vic-anim.service
Wants=vic-cloud.service vic-robot.service
[Service]
ExecStartPre=/bin/sh -c "while [ ! -S /dev/socket/mic_sock ]; do sleep 1; done"
ExecStartPre=/bin/sleep 8
ExecStart=/data/mictap
Restart=always
RestartSec=5
[Install]
WantedBy=multi-user.target
'
  DROPIN='# MBot: load the mic shim into vic-anim (copies raw mic audio to mictap; optional firmware silence).
# Remove this file and reboot to return to stock.
[Service]
Environment=LD_PRELOAD=/data/mbot/libmbotmic.so
'
  TIMER='[Unit]
Description=Run the MBot mic-shim safety check 150 s after boot
[Timer]
OnBootSec=150
[Install]
WantedBy=timers.target
'
  GSVC='[Unit]
Description=MBot mic-shim safety net (auto-removes the shim if vic-anim fails)
[Service]
Type=oneshot
ExecStart=/data/mbot/shim-guard.sh
'
  r 'mount -o remount,rw /'
  put /etc/systemd/system/mictap.service "$MICTAP"
  r 'mkdir -p /etc/systemd/system/vic-anim.service.d'
  put /etc/systemd/system/vic-anim.service.d/mbot-mic.conf "$DROPIN"
  put /etc/systemd/system/mbot-shim-guard.timer "$TIMER"
  put /etc/systemd/system/mbot-shim-guard.service "$GSVC"
  r 'ln -sf /etc/systemd/system/mictap.service /etc/systemd/system/multi-user.target.wants/mictap.service;
     mkdir -p /etc/systemd/system/timers.target.wants;
     ln -sf /etc/systemd/system/mbot-shim-guard.timer /etc/systemd/system/timers.target.wants/mbot-shim-guard.timer;
     sync; mount -o remount,ro /; systemctl daemon-reload'
  r '[ -e /data/mbot/mic_silence ] || touch /data/mbot/mic_silence'  # no firmware wake jump (MBot hears anyway)
  echo "   installed mictap, the shim drop-in and the safety net"
  need_reboot=1
else
  echo "   already installed"
fi

if [ "$need_reboot" = 1 ]; then
  if [ "$YES" = 1 ] || { read -r -p "Reboot him now so the shim loads? [y/N] " a && [ "${a,,}" = y ]; }; then
    r 'reboot' || true
    echo "   rebooting; give him ~2 minutes, then: robot/install.sh $IP --check"
  else
    echo "   reboot him when convenient (the new shim loads then)"
  fi
fi
status
