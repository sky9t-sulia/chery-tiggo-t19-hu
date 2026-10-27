# Tiggo HU — a custom shell for the Chery Tiggo 4 (T19) head unit

A from-scratch replacement for the stock UI of the 2019 Chery Tiggo 4 (platform T19, Shinco i.MX6 DualLite head unit),
written in C on OpenGL ES 2 and running directly on the Vivante framebuffer — no Qt, no Weston.
The stock firmware stays on the unit and can still be booted.

Personal project for one car. Nothing here is affiliated with or endorsed by Chery, Shinco, Neusoft or Google.
Flashing anything to a head unit can brick it — use at your own risk.

## What it does

- **Home / dashboard**, apps grid, settings, display, vehicle info, light and dark themes, English / Ukrainian / Russian UI.
- **Media:** USB music and video (hardware VPU decoding), album art with a blurred art background, FM/AM radio.
- **Bluetooth** through the stock nFore stack: calls, phone book, A2DP music with AVRCP metadata.
- **Android Auto**, wired (USB AOA) and wireless (Bluetooth RFCOMM + 5 GHz Wi-Fi AP on an RTL8811AU dongle):
  video, audio, touch, media metadata and keys, and sensors (speed, night mode, driving status, steering angle, headlights).
- **Car data** from the body MCU over UART (`/dev/ttymxc2`): speed, climate, lights, steering, the rotary knob,
  steering-wheel keys, reverse camera with dynamic guide lines.
- **Lua apps** (`apps/`).
- Boot splash (`boot/huboot`), crash handler with a backtrace, a watchdog that restarts the shell on a crash.

## Layout

| Path | What |
|---|---|
| `src/` | The shell (C) |
| `tools/` | Scripts that run on the HU (start, watchdog, Wi-Fi AP, init), the NAND installer, asset and table generators |
| `boot/` | Boot splash: `boot_static.html` → PNG → `boot_img.h` → `huboot` |
| `wifi/` | RTL8811AU driver patch, hostapd / udhcpd configs, build notes |
| `apps/` | Lua apps |
| `res/`, `icons_png/` | Fonts, wallpapers, demo images, icon masks — inputs for `make assets` |
| `container/` | Podman images: ARM cross toolchain (Ubuntu 14.04, gcc 4.8, glibc 2.17 like the HU) and the PC build |
| `third_party/` | Lua 5.4, stb, libjpeg headers |

## Building

Everything builds in containers (Podman); the host is not touched.

```sh
sh container/build.sh                    # images localhost/tiggo-build and localhost/tiggo-pc
cp src/aa_cert.h.example src/aa_cert.h   # then fill in the Android Auto certificate and key, see below
sh tools/hu.sh assets                    # bake fonts and icons into assets/
sh tools/hu.sh pc                        # PC build: build/pc/hu;  sh tools/hu.sh run  opens it in a Wayland window
SYSROOT=/path/to/hu-rootfs sh tools/hu.sh arm   # HU build: build/arm/hu (stripped) + build/arm/hu.debug
```

The ARM build links against the head unit's own libraries (Vivante EGL/GLES, nFore `libbluelink`), so it needs
a sysroot: the root filesystem dumped from **your** head unit's NAND. Stock firmware is not included here.

**Android Auto certificate.** The phone only accepts a head-unit certificate signed by Google's Automotive Link CA.
`src/aa_cert.h` is git-ignored; `src/aa_cert.h.example` says where the well-known certificate and key come from.

**Wi-Fi driver and hostapd** for wireless Android Auto are built separately, see `wifi/README.md`.

## Installing on the head unit

Needs root SSH access to the head unit (the HU reaches the PC through a phone's USB tethering and a reverse SSH tunnel,
`tools/rescue/`).

```sh
sh tools/nand-install.sh stage   # copy the build to the HU's RAM and verify sha256
sh tools/nand-install.sh nand    # install into NAND (stock shell kept; see the script header for rollback)
```

The stock UI boots instead of this shell when a USB stick with an empty file named `STOCK` in its root is inserted.

## License

MIT — see [LICENSE](LICENSE). Third-party code under `third_party/` keeps its own licenses (Lua — MIT,
stb — public domain / MIT, libjpeg headers — IJG).
