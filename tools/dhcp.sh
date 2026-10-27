#!/bin/sh
# udhcpc: address and route for USB tethering, DNS in /var/run/resolv.conf (RAM; /etc/resolv.conf is a link to it)
[ "$1" = bound ] || [ "$1" = renew ] || exit 0
ifconfig $interface $ip netmask ${subnet:-255.255.255.0} up
while route del default 2>/dev/null; do :; done          # all foreign ones (stock 192.168.7.1 etc.)
route add default gw $router dev $interface
echo "$router" > "$(dirname "$0")/gw.$interface"         # net-usb.sh makes sure the route goes here
: > /var/run/resolv.conf
for d in $dns; do echo "nameserver $d" >> /var/run/resolv.conf; done
echo "DHCP ok ip=$ip gw=$router dns=$dns"
