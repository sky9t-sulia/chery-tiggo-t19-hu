#!/bin/sh
# Install our shell into the HU NAND (run on the PC, over SSH through the tunnel).
#   stage - upload to the HU RAM (/dev/hu-stage) and verify sha256; does not touch NAND;
#   nand  - from /dev/hu-stage into NAND: /home/root/tiggo-hu (hu, assets, hu-run.sh, start.sh), /etc/init.d/weston
#           (the old one - weston.tiggo-stick), /etc/tiggo/net-usb.sh and dhcp.sh (old ones - .bak); sha256 is verified again in NAND.
#           Wireless AA Wi-Fi: tiggo-hu/wifi (driver, hostapd, configs, wifi.sh - started by start.sh).
#           boot screen: /etc/init.d/huboot + link rcS.d/S01huboot (rollback: rm /etc/rcS.d/S01huboot).
#           S08: /etc/init.d/tiggo + link rc5.d/S08tiggo instead of the stock S08shincoservice (it becomes /etc/init.d/shincoservice.stock).
# The root (ubifs) is read-only: rw while writing, then back to ro (also on error).
# Rollback: mount -o remount,rw /; rm /etc/rc5.d/S08tiggo; cp -p /etc/init.d/shincoservice.stock /etc/rc5.d/S08shincoservice;
#        mv /etc/init.d/weston.tiggo-stick /etc/init.d/weston (+ .bak in /etc/tiggo); rm -r /home/root/tiggo-hu
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
SSH="timeout 120 ssh -o UserKnownHostsFile=$HOME/.config/tiggo-hu/known_hosts_hu -o ConnectTimeout=10 -o BatchMode=yes -i $HOME/.config/tiggo-hu/pc_to_hu -p 2200 root@localhost"
SUM='find . -type f | sort | xargs sha256sum'
case "$1" in
stage)
    W=$(mktemp -d)
    mkdir -p "$W/tiggo-hu" "$W/etc"
    cp "$T/build/arm/hu" "$T/tools/hu-run.sh" "$T/tools/start.sh" "$T/tools/console.sh" "$T/boot/huboot" "$W/tiggo-hu/"
    cp -r "$T/assets" "$W/tiggo-hu/"
    # Wi-Fi (wireless AA): the driver and hostapd are built outside git (../wifi, hu/wifi/README.md), configs and script are here
    mkdir "$W/tiggo-hu/wifi"
    WIFI=${WIFI:-$T/../wifi}   # build dir of the driver and hostapd (hu/wifi/README.md)
    cp "$WIFI/aircrack-8812au/88XXau.ko" "$WIFI/hostapd-2.10/hostapd/hostapd" "$WIFI/hostapd-2.10/hostapd/hostapd_cli" \
       "$T/wifi/udhcpd.conf" "$T/tools/wifi.sh" "$W/tiggo-hu/wifi/"
    cp "$T/wifi/hostapd-ap.conf" "$W/tiggo-hu/wifi/hostapd.conf"
    cp "$T/tools/weston.nand" "$W/etc/weston"; cp "$T/tools/tiggo.init" "$W/etc/tiggo"; cp "$T/tools/huboot.init" "$W/etc/huboot"; cp "$T/tools/net-usb.sh" "$T/tools/dhcp.sh" "$W/etc/"
    ( cd "$W" && LC_ALL=C sh -c "$SUM" ) > "$W.sum" && mv "$W.sum" "$W/pc.sum"   # not inside - so it does not include itself
    echo "version: $(strings "$W/tiggo-hu/hu" | grep hu-build)"
    ( cd "$W" && tar cf - tiggo-hu etc pc.sum ) | $SSH "rm -rf /dev/hu-stage; mkdir /dev/hu-stage && cd /dev/hu-stage && tar xf - &&
        mv pc.sum ../hu-stage.pc.sum && $SUM > ../hu-stage.sum && mv ../hu-stage.pc.sum pc.sum && mv ../hu-stage.sum ram.sum &&
        cmp pc.sum ram.sum && echo \"RAM: $(wc -l < "$W/pc.sum") files, sha256 match\""
    rm -r "$W"
    ;;
nand)
    $SSH 'cd /dev/hu-stage && [ -f ram.sum ] && cmp pc.sum ram.sum || { echo "no verified stage"; exit 1; }
        sh -n tiggo-hu/start.sh && sh -n tiggo-hu/hu-run.sh && sh -n tiggo-hu/wifi/wifi.sh && sh -n etc/weston && sh -n etc/tiggo && sh -n etc/huboot && sh -n etc/net-usb.sh && sh -n etc/dhcp.sh || exit 1
        mount -o remount,rw / || exit 1
        ( set -e
          rm -rf /home/root/tiggo-hu.new; cp -r tiggo-hu /home/root/tiggo-hu.new
          chmod 755 /home/root/tiggo-hu.new/hu /home/root/tiggo-hu.new/huboot /home/root/tiggo-hu.new/*.sh /home/root/tiggo-hu.new/wifi/hostapd /home/root/tiggo-hu.new/wifi/hostapd_cli /home/root/tiggo-hu.new/wifi/wifi.sh
          [ -f /etc/init.d/weston.tiggo-stick ] || cp -p /etc/init.d/weston /etc/init.d/weston.tiggo-stick
          for f in net-usb.sh dhcp.sh; do [ -f /etc/tiggo/$f.bak ] || cp -p /etc/tiggo/$f /etc/tiggo/$f.bak; done
          cp etc/weston /etc/init.d/weston.new; chmod 755 /etc/init.d/weston.new
          cp etc/tiggo /etc/init.d/tiggo.new; chmod 755 /etc/init.d/tiggo.new
          cp etc/huboot /etc/init.d/huboot.new; chmod 755 /etc/init.d/huboot.new
          cp etc/net-usb.sh /etc/tiggo/net-usb.sh.new; cp etc/dhcp.sh /etc/tiggo/dhcp.sh.new; chmod 700 /etc/tiggo/*.new
          sync
          ( cd /home/root/tiggo-hu.new && '"$SUM"' ) | sed "s| \./| ./tiggo-hu/|" > /dev/hu-stage/nand.sum
          for f in weston:/etc/init.d/weston.new tiggo:/etc/init.d/tiggo.new huboot:/etc/init.d/huboot.new net-usb.sh:/etc/tiggo/net-usb.sh.new dhcp.sh:/etc/tiggo/dhcp.sh.new; do
              echo "$(sha256sum < ${f#*:} | cut -d" " -f1)  ./etc/${f%%:*}" >> /dev/hu-stage/nand.sum; done
          sort -k2 /dev/hu-stage/nand.sum > /dev/hu-stage/nand.sorted; sort -k2 pc.sum > /dev/hu-stage/pc.sorted
          cmp /dev/hu-stage/pc.sorted /dev/hu-stage/nand.sorted
          rm -rf /home/root/tiggo-hu; mv /home/root/tiggo-hu.new /home/root/tiggo-hu
          mv -f /etc/init.d/weston.new /etc/init.d/weston
          mv -f /etc/init.d/tiggo.new /etc/init.d/tiggo
          mv -f /etc/init.d/huboot.new /etc/init.d/huboot
          ln -sf ../init.d/huboot /etc/rcS.d/S01huboot          # boot screen - first
          # S08: stock shincoservice (a regular file in rc5.d) -> /etc/init.d/shincoservice.stock, our tiggo in its place
          [ -f /etc/init.d/shincoservice.stock ] || cp -p /etc/rc5.d/S08shincoservice /etc/init.d/shincoservice.stock
          ln -sf ../init.d/tiggo /etc/rc5.d/S08tiggo
          rm -f /etc/rc5.d/S08shincoservice
          mv -f /etc/tiggo/net-usb.sh.new /etc/tiggo/net-usb.sh; mv -f /etc/tiggo/dhcp.sh.new /etc/tiggo/dhcp.sh ); r=$?
        sync; mount -o remount,ro / || echo "/ stayed rw until reboot (data written: the running start.sh holds its old file)"
        grep " / " /proc/mounts
        [ $r -eq 0 ] && echo "NAND: written, sha256 match ($(wc -l < /dev/hu-stage/pc.sum)files)" || echo "NAND: ERROR r=$r"
        ls -la /home/root/tiggo-hu /etc/init.d/weston* /etc/init.d/tiggo /etc/init.d/huboot /etc/rcS.d/S01huboot /etc/init.d/shincoservice.stock /etc/rc5.d/S08* /etc/tiggo/*.sh*; exit $r'
    ;;
*) echo "usage: $0 stage|nand"; exit 1 ;;
esac
