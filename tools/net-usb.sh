#!/bin/sh
# Phone USB tethering (runs on the HU, everything in RAM): Android in "USB modem" mode -> rndis_host / cdc_ncm / cdc_ether.
# Every 2 s it looks for such an interface and keeps udhcpc on it (address, route, DNS - script dhcp.sh).
DIR=${HU_DIR:-/dev/hu}
modprobe rndis_host 2>/dev/null; modprobe cdc_ncm 2>/dev/null
# regular USB (ci_hdrc.1, bus 2): with USB autosuspend (runtime PM, default auto) a phone in modem mode
# drops off a second after connecting; "on" keeps it (verified 2026-10-24). For all ports.
# Someone sets it back to auto after startup (2026-10-24: the ci_hdrc.1 controller fell asleep - the regular port saw neither the phone
# nor the USB stick until "on") - check on every pass.
usb_awake() {
    for c in /sys/bus/usb/devices/usb*/power/control /sys/devices/soc0/soc.1/2100000.aips-bus/2184*.usb/power/control \
             /sys/devices/soc0/soc.1/2100000.aips-bus/2184*.usb/ci_hdrc.*/power/control; do
        [ -f "$c" ] && [ "$(cat "$c")" != on ] && echo on > "$c" 2>/dev/null
    done
}
while true; do
    usb_awake
    for n in /sys/class/net/*; do
        i=${n##*/}
        drv=$(basename "$(readlink "$n/device/driver" 2>/dev/null)" 2>/dev/null)
        case "$drv" in rndis_host|cdc_ncm|cdc_ether) ;; *) continue ;; esac
        # the stock /etc/network/interfaces puts a static 192.168.7.2, gateway 192.168.7.1 on usb0 (a stub for
        # the HU's USB gadget). The phone became usb0 before the gadget (our shell starts without the stock one) - the address and route are foreign,
        # no connectivity. Reset and get the address from the phone again (if the phone itself issued 192.168.7.x - leave it)
        case "$(ifconfig "$i" 2>/dev/null)" in *"inet addr:192.168.7."*)
            if [ "$(cat "$DIR/gw.$i" 2>/dev/null)" != 192.168.7.1 ]; then
                echo "net-usb: $i - stock 192.168.7.x, reset" >&2
                ifconfig "$i" 0.0.0.0 2>/dev/null; route del default gw 192.168.7.1 2>/dev/null
                [ -f "$DIR/udhcpc.$i.pid" ] && kill -USR1 "$(cat "$DIR/udhcpc.$i.pid")" 2>/dev/null   # renew
            fi ;;
        esac
        [ -f "$DIR/udhcpc.$i.pid" ] && kill -0 "$(cat "$DIR/udhcpc.$i.pid")" 2>/dev/null && continue
        echo "net-usb: $i ($drv) - DHCP" >&2
        ifconfig "$i" up
        udhcpc -i "$i" -b -p "$DIR/udhcpc.$i.pid" -s "$DIR/dhcp.sh" >/dev/null 2>&1
    done
    for f in "$DIR"/udhcpc.*.pid; do                 # phone disconnected - remove its udhcpc
        [ -f "$f" ] || continue
        i=${f#"$DIR"/udhcpc.}; i=${i%.pid}
        [ -e "/sys/class/net/$i" ] || { kill "$(cat "$f")" 2>/dev/null; rm -f "$f" "$DIR/gw.$i"; echo "net-usb: $i gone" >&2; }
    done
    for f in "$DIR"/gw.*; do                         # default route - via the phone (the stock ifup may have overridden it)
        [ -f "$f" ] || continue
        i=${f#"$DIR"/gw.}; gw=$(cat "$f")
        [ -e "/sys/class/net/$i" ] || { rm -f "$f"; continue; }
        [ "$(route -n | awk '$1 == "0.0.0.0" { print $2 " " $8; exit }')" = "$gw $i" ] && continue
        while route del default 2>/dev/null; do :; done
        route add default gw "$gw" dev "$i" && echo "net-usb: default -> $gw ($i)" >&2
    done
    sleep 2
done
