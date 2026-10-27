#!/bin/sh
# Wi-Fi access point for wireless Android Auto: TP-Link Archer T2U Nano dongle (2357:011f, RTL8811AU) - driver 88XXau.ko
# (aircrack 5.6.4.2 for kernel 3.10, hu/wifi/README.md), hostapd 2.10 (5 GHz, WPA2), udhcpd 10.42.0.x. Everything in RAM (/dev/wifi);
# hu reads the network from /dev/wifi/hostapd.conf and hands it to the phone over Bluetooth (aa_wifi.c).
# Run from start.sh in the background, after hu and SSH (wait 25 s from power-on) - the driver and then the AP right away (like aawgd, never torn down);
# "wifi.sh up" (from /dev/wifi) is also called by hu - if there is no AP yet. Bootloop protection: during insmod there is a marker in NAND
# (/mnt/data1/hu/wifi.try); if left over from the previous boot (the driver hung the kernel) - we do not bring Wi-Fi up this time, the marker
# is removed (the next boot will try again). No dongle - exit quietly.
SRC=$(cd "$(dirname "$0")" && pwd)
W=/dev/wifi
TRY=/mnt/data1/hu/wifi.try
KEY=/mnt/data1/hu/wifi.key                 # AP password (persistent)
log() { echo "wifi: $*" >&2; }

case "$1" in
up)     # AP - like WirelessAndroidAutoDongle (aawgd; owner 2026-10-26 "1:1"): brought up at boot (below) and
        # never torn down; hu (aa_wifi.c) calls "up" for every phone arrival - already up, we do nothing.
        # Two "up" at once (boot and hu) - serialized (mkdir is the lock)
    n=0; while ! mkdir $W/.uplock 2>/dev/null && [ $n -lt 40 ]; do sleep 0.5; n=$((n + 1)); done
    trap 'rmdir $W/.uplock 2>/dev/null' EXIT
    [ -d /sys/class/net/wlan0 ] || { log "no wlan0"; exit 1; }
    ip addr add 10.42.0.1/24 dev wlan0 2>/dev/null
    ip link set wlan0 up
    if ! pidof hostapd > /dev/null; then
        # network: CheryTiggo4 (owner 2026-10-26), the password is persistent, as on production HUs: random once, stored in
        # NAND (the phone gets it over Bluetooth, no need to type it). Earlier it was new on every bring-up - after the car was switched
        # off/on the phone, with CheryTiggo4 just remembered (old password), connected twice, left the air, AA
        # stalled and reconnected (in the car 2026-10-26); the first session after boot - without this
        K=$(cat $KEY 2>/dev/null)
        if [ ${#K} -ne 20 ]; then
            K=$(tr -dc A-Za-z0-9 < /dev/urandom | head -c 20)
            [ ${#K} -eq 20 ] || { log "no random password"; exit 1; }
            echo "$K" > $KEY.new && sync && mv $KEY.new $KEY && sync
        fi
        sed -i "s/^ssid=.*/ssid=CheryTiggo4/; s/^wpa_passphrase=.*/wpa_passphrase=$K/" $W/hostapd.conf
        $W/hostapd -B $W/hostapd.conf > $W/hostapd.log 2>&1 || { log "hostapd: $?"; exit 1; }
        : > $W/udhcpd.leases
    fi
    n=0; while [ $n -lt 20 ] && ! $W/hostapd_cli -p $W/ctrl status 2>/dev/null | grep -q "^state=ENABLED"; do sleep 0.5; n=$((n + 1)); done
    pidof udhcpd > /dev/null || udhcpd $W/udhcpd.conf
    $W/hostapd_cli -p $W/ctrl status 2>/dev/null | grep -q "^state=ENABLED" || { log "AP did not come up"; exit 1; }
    # AP events (connected / disconnected / wrong password - AP-STA-POSSIBLE-PSK-MISMATCH) - into the analysis log
    # of wireless AA: owner 2026-10-26 - after bye-bye the phone seems to connect twice
    pidof hostapd_cli > /dev/null || $W/hostapd_cli -p $W/ctrl -i wlan0 -B -a $W/wifi.sh
    echo "$(cut -d' ' -f1 /proc/uptime) ap: up" >> /dev/hu/wifi-diag.log
    log "AP up"
    exit 0 ;;
diag)   # snapshot for analysing wireless AA stalls (called by hu, aa.c wifi_diag): dongle, AP, phone
    P=/proc/net/rtl88XXau/wlan0
    {
        echo "=== $2 $(cut -d' ' -f1 /proc/uptime)"
        grep wlan0 /proc/net/dev; grep -A1 "^Tcp:" /proc/net/snmp | tail -1
        $W/hostapd_cli -p $W/ctrl all_sta 2>&1 | grep -e "^[0-9a-f][0-9a-f]:" -e flags -e packets -e inactive -e signal -e connected -e timeout
        cat $P/trx_info $P/rx_signal 2>&1
        ping -c 2 -W 1 10.42.0.10 2>&1 | tail -2
        cat $P/trx_info 2>&1 | grep -e free_ -e hwq -e page -e urb
        echo "### sta"; awk '/^sta.s macaddr:/{p=$0 !~ /ff:ff:ff:ff:ff:ff|3c:78:95:37:56:ca/} p' $P/all_sta_info 2>&1   # phone station
    } >> /dev/hu/wifi-diag.log 2>&1
    exit 0 ;;
wlan0)  # AP event from hostapd_cli -a: "wlan0 <event> [MAC ...]"
    shift; echo "$(cut -d' ' -f1 /proc/uptime) ap: $*" >> /dev/hu/wifi-diag.log
    # phone connected - a full driver snapshot after 3 s (the first connection after boot is the reference, after
    # an AP restart - compare; owner 2026-10-26: everything breaks after the first AP teardown)
    [ "$1" = EAPOL-4WAY-HS-COMPLETED ] && { sleep 3; sh $W/wifi.sh full; } &
    exit 0 ;;
full)
    P=/proc/net/rtl88XXau/wlan0
    {
        echo "=== full $(cut -d' ' -f1 /proc/uptime)"
        for f in ap_info adapters_status all_sta_info macid_info sec_info sec_cam mlmext_state fwstate linked_info_dump rx_stat tx_stat; do
            echo "### $f"; timeout 3 cat $P/$f 2>&1 | head -80
        done
        echo "### hostapd"; $W/hostapd_cli -p $W/ctrl status 2>&1; $W/hostapd_cli -p $W/ctrl all_sta 2>&1
        echo "### ping"; ping -c 3 -W 1 10.42.0.10 2>&1 | tail -2
    } >> /dev/hu/wifi-diag.log 2>&1
    exit 0 ;;
down)   # manual (hu no longer tears the AP down)
    killall hostapd_cli hostapd udhcpd 2>/dev/null
    echo "$(cut -d' ' -f1 /proc/uptime) ap: down" >> /dev/hu/wifi-diag.log
    log "AP down"
    exit 0 ;;
esac

# boot: driver and AP
while [ "$(cut -d. -f1 /proc/uptime)" -lt 25 ]; do sleep 1; done
grep -qil 2357 /sys/bus/usb/devices/*/idVendor 2>/dev/null || { log "no dongle"; exit 0; }
if [ -f $TRY ]; then
    rm -f $TRY; sync
    log "previous insmod attempt did not finish - no Wi-Fi this time"
    exit 0
fi
mkdir -p $W && cp "$SRC"/88XXau.ko "$SRC"/hostapd "$SRC"/hostapd_cli "$SRC"/hostapd.conf "$SRC"/udhcpd.conf "$SRC"/wifi.sh $W/ || exit 1
chmod 755 $W/hostapd $W/hostapd_cli
if ! grep -q "^88XXau " /proc/modules; then
    : > $TRY; sync
    insmod $W/88XXau.ko rtw_country_code=GE || log "insmod: $?"
    n=0; while [ $n -lt 20 ] && [ ! -d /sys/class/net/wlan0 ]; do sleep 0.5; n=$((n + 1)); done
    rm -f $TRY; sync
fi
[ -d /sys/class/net/wlan0 ] || { log "no wlan0"; exit 0; }
log "driver loaded"
exec sh $W/wifi.sh up                                     # AP - immediately and permanently (aawgd)
