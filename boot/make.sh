#!/bin/sh
# huboot boot screen: design snapshot (boot_static.html, headless Chrome on the PC) with the shell version from src/version.h ->
# boot_img.h -> huboot (ARM, in the tiggo-build container). usage: hu/boot/make.sh
set -e
cd "$(dirname "$0")"
V=$(sed -n 's/^#define HU_VERSION "\(.*\)"/\1/p' ../src/version.h)
sed "s/@VERSION@/$V/" boot_static.html > .boot_v.html                  # alongside: relative paths to the font and logo
google-chrome --headless=new --disable-gpu --hide-scrollbars --window-size=1024,600 --allow-file-access-from-files \
  --virtual-time-budget=3000 --screenshot="$PWD/boot_static.png" "file://$PWD/.boot_v.html" > /dev/null 2>&1
rm -f .boot_v.html
python3 make_img.py boot_static.png boot_img.h
HU=$(cd .. && pwd); SYSROOT=${SYSROOT:-$(echo "$HU"/../extracted/rootfs/*/rootfs)}
podman run --rm --cgroup-manager=cgroupfs -v "$HU":/work:Z -v "$SYSROOT":/sysroot:ro,Z -w /work/boot localhost/tiggo-build \
  arm-linux-gnueabihf-gcc --sysroot=/sysroot -O2 -s -Wall -Wextra -I../src -o huboot huboot.c ../src/mcu.c -lm
echo "huboot $V: $(wc -c < huboot) bytes"
