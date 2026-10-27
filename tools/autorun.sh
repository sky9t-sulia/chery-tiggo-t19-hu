#!/bin/sh
# Our shell instead of the stock one. Called in the background from /etc/init.d/weston (start) when this file is found on the USB stick
# (tiggo/hu/autorun.sh, wait up to 60 s); without the stick the HU works as stock. Everything runs from RAM (/dev/hu), nothing is written to NAND.
SRC=$(cd "$(dirname "$0")" && pwd)
DIR=/dev/hu
[ -d $DIR ] && exit 0                                   # already running
# the stock UI has already started (the stick is mounted later than weston) - stop it; ShincoService holds the MCU UART
killall startup.sh carplay.sh 2>/dev/null
killall -9 ShellApp 2>/dev/null
killall weston AppleCarPlay ShincoService YQ_SSPMain 2>/dev/null
killall GPSService 2>/dev/null
mkdir -p $DIR && cp -r "$SRC"/* $DIR/
chmod +x $DIR/hu $DIR/*.sh
# network (phone USB tethering) and remote access - first: sshd (127.0.0.1:2022, key auth) + reverse tunnel to the PC
# (address - ssh/pc_addr, localhost:2201 on the PC). Always, independent of the NAND tiggo-rescue service (it has its own 22 / 2200):
# if one does not come up, the other works.
pidof -x net-usb.sh >/dev/null || HU_DIR=$DIR nohup $DIR/net-usb.sh 2>>$DIR/net.log >/dev/null &   # one network shared by both
if [ -f $DIR/ssh/sshd_config ]; then
    chmod 700 $DIR/ssh; chmod 600 $DIR/ssh/*; chmod 700 $DIR/ssh/tunnel.sh
    /usr/sbin/sshd -f $DIR/ssh/sshd_config
    nohup $DIR/ssh/tunnel.sh > $DIR/ssh/tunnel.log 2>&1 &
fi
# black box: every 10 s write uptime and the MCU log to the stick - after a full shutdown it shows how long the HU lived while asleep
# (the HU clock starts from the same time after every boot - the name uses the boot number)
BB="$SRC/../blackbox"; mkdir -p "$BB"; n=$(cat "$BB/n" 2>/dev/null); n=$((${n:-0} + 1)); echo $n > "$BB/n"; B="$BB/boot_$n"
# + remote access state (the NAND service /dev/tiggo and our own): why it did not come up is visible even without SSH
( while :; do
      echo "$(date +%s) up=$(cut -d' ' -f1 /proc/uptime)" >> "$B.txt"
      cp $DIR/mcu.log "$B.mcu.log" 2>/dev/null
      { ps; ifconfig; route -n; netstat -ltn; for f in /dev/tiggo/*.log $DIR/net.log $DIR/ssh/tunnel.log; do echo "== $f"; tail -20 $f; done
        dmesg | grep -iE "rndis|cdc|usb[0-9]" | tail -10; } > "$B.net.txt" 2>&1
      sync; sleep 10
  done ) &
HU_DIR=$DIR nohup $DIR/hu-run.sh > $DIR/log.txt 2>&1 &
( sleep 15; killall YQ_SSPMain 2>/dev/null ) &           # applink starts 12 s after weston
