#!/bin/sh
# Terminal on the HU screen (Terminal app, USB keyboard): the screen shows the kernel consoles, a root sh on the 2nd console;
# exit - the screen goes back to the shell. hu keeps running all this time (MCU keep-alive, audio) but does not draw (SIGUSR1 / SIGUSR2).
VT=/sys/class/vtconsole/vtcon1                         # frame buffer device
[ "$(cat $VT/name 2>/dev/null | grep -c 'frame buffer')" = 1 ] || exit 1
pidof hu > /dev/null || exit 1
# a previous shell may remain on the 2nd console (it was cut off) - close it, otherwise openvt: "vt 2 is in use"
for p in /proc/[0-9]*; do [ "$(cut -d' ' -f7 $p/stat 2>/dev/null)" = 1026 ] && kill -HUP ${p#/proc/} 2>/dev/null; done
kill -USR1 $(pidof hu); sleep 0.3
echo 1 > $VT/bind
printf '\033c' > /dev/tty2                            # console reset: black screen, no text from the previous shell
openvt -f -c 2 -s -w -- /bin/sh -l
chvt 1
echo 0 > $VT/bind
kill -USR2 $(pidof hu)
