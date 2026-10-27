# 5 GHz Wi-Fi on the head unit (wireless Android Auto)

TP-Link Archer T2U Nano dongle (`2357:011f`, RTL8811AU). HU kernel: NXP `3.10.53-1.1.0_ga`, gcc 4.8.2, `MODVERSIONS`.

Building the driver (the `wifi/` folder in the repo root is outside git, everything is downloaded again):
1. Kernel: `git clone --depth 1 --branch rel_imx_3.10.53_1.1.0_ga_caf https://github.com/nxp-imx/linux-imx.git`,
config - `/proc/config.gz` from the HU -> `kout/.config`, `make O=kout olddefconfig modules_prepare` in the
`Containerfile.kbuild` container (Ubuntu 16.04, gcc-4.8-arm-linux-gnueabihf).
2. `Module.symvers`: checksums from the HU kernel image (`/dev/mtd2ro` -> LZO at offset 0x1770 -> Image @0x80008000,
`__ksymtab*`/`__kcrctab*` tables at the addresses from `/proc/kallsyms`). Checked against the stock modules - matches.
3. Driver: aircrack-ng/rtl8812au (commit in `aircrack-8812au.commit`) + `aircrack-8812au-3.10.patch`,
   `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KSRC=kout KVER=3.10.53-1.1.0_ga CONFIG_PLATFORM_I386_PC=n CONFIG_PLATFORM_ARM_RPI=y`.
Loading: `insmod 88XXau.ko rtw_country_code=GE`. (The old abperiasamy v4.3.14 cannot hold an access point: frames from
the phone do not arrive.)
4. hostapd 2.10 (`hostapd.config`, libnl-3 from the HU firmware, built-in crypto). Access point: `hostapd-ap.conf` (5 GHz,
channel 36 - legal in GE/UA/DE), addresses - `udhcpd.conf` (busybox).

Verified 2026-10-25 in the car (RAM only): S24 Ultra connected and got 10.42.0.10.
