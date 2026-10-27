#!/bin/sh
# Our shell from NAND (/home/root/tiggo-hu). Called in the background from /etc/init.d/tiggo (S08, instead of the stock S08shincoservice):
# hardware is already ready, there is no stock software at all (no ShincoService, no cameradetect, no weston) - straight to hu from RAM (/dev/hu).
# Does not write to NAND.
# Back to stock (until reboot): hu crashed 5 times in a minute (hu-run.sh gave up) - the whole stock stack ("tiggo stock" +
# weston). The STOCK stick is checked by /etc/init.d/tiggo at boot (in parallel).
# SSH and network over the phone - the tiggo-rescue service (/etc/tiggo), independent of the shell.
SRC=$(cd "$(dirname "$0")" && pwd)
DIR=/dev/hu
LOCK=/dev/hu.stock                                      # exists - stock UI
[ -d $DIR ] || [ -d $LOCK ] && exit 0                   # already running / stock selected
to_stock() {
    mkdir $LOCK 2>/dev/null || return 0                 # only once
    echo "start.sh: $1 - stock UI" >&2
    killall hu 2>/dev/null; sleep 1; killall -9 hu 2>/dev/null
    /etc/init.d/tiggo stock                             # stock S08 programs without hardware: ShincoService (MCU keep-alive), camera, conver
    TIGGO_WESTON=1 /etc/init.d/weston start
}
mkdir -p $DIR && cp -r "$SRC"/* $DIR/                  # and the assets - into RAM (owner: running from NAND may stutter)
chmod +x $DIR/hu $DIR/*.sh
[ -f $DIR/wifi/wifi.sh ] && ( sh $DIR/wifi/wifi.sh > $DIR/wifi.log 2>&1 & )   # Wi-Fi for wireless AA - in the background, waits 25 s by itself
HU_DIR=$DIR $DIR/hu-run.sh > $DIR/log.txt 2>&1; rc=$?
[ $rc -ne 0 ] && to_stock "hu-run.sh rc=$rc" >> $DIR/log.txt 2>&1
exit 0
