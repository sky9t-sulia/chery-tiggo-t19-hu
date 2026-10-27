#!/bin/sh
# Install tiggo-rescue into the HU NAND (run on the PC, over SSH through the tunnel). Writes only:
#   /etc/tiggo/ (network/tunnel scripts and keys from ~/.config/tiggo-hu/hu), /etc/init.d/tiggo-rescue, /etc/rc5.d/S03tiggo-rescue.
# Does not start the service (sshd and the tunnel are currently run by autorun) - it will work from the next boot.
# Remove: rm -r /etc/tiggo /etc/init.d/tiggo-rescue /etc/rc5.d/S03tiggo-rescue
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
K=$HOME/.config/tiggo-hu/hu
SSH="timeout 60 ssh -o UserKnownHostsFile=$HOME/.config/tiggo-hu/known_hosts_hu -o ConnectTimeout=6 -o BatchMode=yes -i $HOME/.config/tiggo-hu/pc_to_hu -p 2200 root@localhost"
W=$(mktemp -d)
cp "$T/net-usb.sh" "$T/dhcp.sh" "$T/rescue/sshd_config" "$K/host_key" "$K/authorized_keys" "$K/known_hosts" "$K/pc_addr" "$K/tun_key" "$K/tunnel.sh" "$W/"
cp "$T/rescue/tiggo-rescue" "$W/tiggo-rescue.init"
# the root (ubifs) is mounted read-only: rw while writing, then back to ro (also on error)
( cd "$W" && tar cf - . ) | $SSH 'mount -o remount,rw / || exit 1
    ( set -e; mkdir -p /etc/tiggo; cd /etc/tiggo; tar xf -; mv tiggo-rescue.init /etc/init.d/tiggo-rescue
      chmod 755 /etc/init.d/tiggo-rescue; chmod 700 /etc/tiggo; chmod 600 /etc/tiggo/*; chmod 700 /etc/tiggo/*.sh
      ln -sf ../init.d/tiggo-rescue /etc/rc5.d/S03tiggo-rescue ); r=$?
    sync; mount -o remount,ro /
    grep " / " /proc/mounts | grep ubifs; ls -la /etc/tiggo /etc/rc5.d/S03tiggo-rescue; md5sum /etc/init.d/tiggo-rescue; exit $r'
md5sum "$T/rescue/tiggo-rescue"
rm -r "$W"
