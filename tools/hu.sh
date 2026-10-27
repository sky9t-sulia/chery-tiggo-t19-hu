#!/bin/sh
# Everything through containers (the host is untouched):
#   tools/hu.sh assets   - bake fonts/icons (PC)
#   tools/hu.sh pc       - build for PC;    tools/hu.sh run   - run in a window on the PC (Wayland, GPU)
#   tools/hu.sh asan     - build with leak/memory checks (PC): build/pc/hu-asan
#   tools/hu.sh arm      - build for the HU (SYSROOT = the HU root filesystem, default ../extracted/rootfs/*/rootfs)
set -e
cd "$(dirname "$0")/.."
HU=$PWD
SYSROOT=${SYSROOT:-$(echo "$HU"/../extracted/rootfs/*/rootfs)}
PC="podman run --rm -v $HU:/work:Z -w /work localhost/tiggo-pc"
GUI="podman run --rm -e WAYLAND_DISPLAY=wayland-0 -e XDG_RUNTIME_DIR=/tmp/xdg -e HU_ASSETS=/work/assets \
     -v $XDG_RUNTIME_DIR/wayland-0:/tmp/xdg/wayland-0 --device /dev/dri --security-opt label=disable \
     -v $HU:/work -w /work localhost/tiggo-pc"
case "$1" in
  assets) $PC make assets ;;
  pc)     $PC make pc ;;
  run)    $GUI build/pc/hu ;;
  asan)   $PC make pc-asan ;;
  arm)    podman run --rm -v "$HU":/work:Z -v "$SYSROOT":/sysroot:ro,Z -w /work localhost/tiggo-build make arm ;;
  *) echo "usage: $0 assets|pc|run|asan|arm"; exit 2 ;;
esac
