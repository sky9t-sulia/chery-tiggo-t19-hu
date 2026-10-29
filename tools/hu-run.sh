#!/bin/sh
# Renderer watchdog on the HU (runs on the HU). Writes nothing to NAND - everything is in RAM (/dev/hu).
#  - virtual memory limit for the process so it cannot eat the system RAM;
#  - restarts on a crash; 5 crashes in a minute - gives up (exit 1: start.sh launches the stock UI).
DIR=${HU_DIR:-/dev/hu}
export HU_ASSETS=$DIR/assets FB_MULTI_BUFFER=2
# the kernel text console (fbcon) draws a blinking cursor right into fb0 (a black square top left, frame glitches) -
# disable the blinking and detach the console from the screen (sysfs, until reboot only). Before every hu start:
# right after the stock weston the detach may fail.
fbcon_off() {
    echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null
    for v in /sys/class/vtconsole/vtcon*; do grep -q "frame buffer" "$v/name" 2>/dev/null && echo 0 > "$v/bind" 2>/dev/null; done
}
# phone USB tethering - a separate process (tools/net-usb.sh)
[ -x "$DIR/net-usb.sh" ] && ! pidof -x net-usb.sh >/dev/null && "$DIR/net-usb.sh" 2>>"$DIR/net.log" &
# crash: the log (with the stack - main.c on_crash) goes to the USB stick if inserted: in RAM it is lost when the HU powers off
# (owner 2026-10-26). We do not write to NAND. The watchdog does not wait for the stick (broken / hung sync - otherwise hu would not restart and
# the MCU without keep-alive would reboot the HU): snapshot goes to RAM immediately, to the stick in the background. Folder hu-crash, name - HU date + time
# since boot
crash_save() {
    s="$DIR/crash-$(cut -d. -f1 /proc/uptime)"
    cp "$DIR/log.txt" "$s-log.txt" 2>/dev/null; tail -c 65536 "$DIR/mcu.log" > "$s-mcu.txt" 2>/dev/null
    (
        awk '$1 ~ /^\/dev\/sd/ && $2 ~ /^\/media\/usb/ && $4 ~ /^rw/ { print $2 }' /proc/mounts | while read -r m; do
            f="$m/hu-crash/$(date +%Y%m%d-%H%M%S)-${s##*/crash-}"
            mkdir -p "$m/hu-crash" && cp "$s-log.txt" "$f-log.txt" && cp "$s-mcu.txt" "$f-mcu.txt" && sync
        done
    ) > /dev/null 2>&1 &
}
ulimit -v 524288
n=0; t0=$(date +%s)
# Update without a pause: the new binary is uploaded as hu.new (the old one keeps running), then "killall hu" -
# the watchdog swaps it in and starts it right away. Important: the MCU reboots the HU after ~15 s without keep-alive from hu.
while true; do
    [ -f "$DIR/hu.new" ] && mv "$DIR/hu.new" "$DIR/hu" && chmod +x "$DIR/hu"
    fbcon_off
    [ -d /dev/hu.stock ] && exit 0                # stock UI selected (STOCK stick / tiggo.init stock()) - never start hu again
    "$DIR/hu"; rc=$?
    [ -d /dev/hu.stock ] && exit 0                # hu was stopped for the stock UI - do not restart it
    [ -f "$DIR/hu.new" ] && continue              # stopped for an update - go straight to the new one
    [ $rc -eq 0 ] && exit 0                       # normal exit (SIGTERM/SIGINT)
    now=$(date +%s)
    [ $((now - t0)) -gt 60 ] && { n=0; t0=$now; }
    n=$((n + 1))
    echo "hu-run: hu exited rc=$rc (crash $n)" >&2
    crash_save
    [ $n -ge 5 ] && { echo "hu-run: too many crashes, giving up" >&2; exit 1; }
    sleep 1
done
