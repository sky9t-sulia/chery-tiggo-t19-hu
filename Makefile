# hu - our own renderer for the HU shell. Builds run in containers (see tools/hu.sh):
#   make pc        - PC (Wayland + Mesa), build/pc/hu
#   make pc-asan   - PC with AddressSanitizer/LeakSanitizer (leak and memory corruption hunting)
#   make arm       - HU (ARM, Vivante fbdev), build/arm/hu  [in the tiggo-build container, HU sysroot in /sysroot]
#   make assets    - bake fonts and icons (in the tiggo-pc container)
SRC      := src/main.c src/ui.c src/media.c src/comp.c src/theme.c src/scr_dash.c src/scr_apps.c src/scr_music.c \
            src/scr_video.c src/scr_radio.c src/scr_camera.c src/scr_display.c src/scr_phone.c \
            src/scr_settings.c src/scr_usb.c src/scr_vehicle.c src/scr_lua.c src/lua_app.c src/player.c src/gfx.c src/text.c \
            src/icons.c src/anim.c src/mcu.c src/fm1388.c src/cam.c src/i18n.c src/standby.c src/settings.c \
            src/aa.c src/aa_usb.c src/aa_tls.c src/aa_pb.c src/aa_view.c src/aa_wifi.c
CWARN    := -Wall -Wextra -Wno-missing-field-initializers
CFLAGS   ?= -O2
CFLAGS   += -std=gnu99 $(CWARN) -Isrc -Ibuild/gen
ARM_CC   := arm-linux-gnueabihf-gcc --sysroot=/sysroot
XDG_XML  := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

# Lua 5.4 (third_party/lua, without lua.c/luac.c) - a separate library per build: not rebuilt every time
LUA_OBJ   = $(patsubst third_party/lua/%.c,build/$(1)/lua/%.o,$(wildcard third_party/lua/*.c))
LUA_FLAGS := -O2 -std=gnu99 -DLUA_USE_POSIX
build/pc/lua/%.o: third_party/lua/%.c
	@mkdir -p $(@D)
	$(CC) $(LUA_FLAGS) -c $< -o $@
build/asan/lua/%.o: third_party/lua/%.c
	@mkdir -p $(@D)
	$(CC) $(LUA_FLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -c $< -o $@
build/arm/lua/%.o: third_party/lua/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(LUA_FLAGS) -c $< -o $@
build/pc/liblua.a: $(call LUA_OBJ,pc)
	ar rcs $@ $^
build/asan/liblua.a: $(call LUA_OBJ,asan)
	ar rcs $@ $^
build/arm/liblua.a: $(call LUA_OBJ,arm)
	arm-linux-gnueabihf-ar rcs $@ $^

build/gen/xdg-shell-client-protocol.h:
	mkdir -p build/gen
	wayland-scanner client-header $(XDG_XML) $@
build/gen/xdg-shell-protocol.c:
	mkdir -p build/gen
	wayland-scanner private-code $(XDG_XML) $@

# Android Auto certificate + key: not in git (see src/aa_cert.h.example)
src/aa_cert.h:
	@echo "src/aa_cert.h is missing: copy src/aa_cert.h.example and fill in the AA certificate and key (see the comment there)" >&2; exit 1

PC_SRC := $(SRC) src/platform_wl.c src/bt_none.c build/gen/xdg-shell-protocol.c
PC_LIB := -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lpthread -ldl -lm

pc: src/aa_cert.h build/gen/xdg-shell-client-protocol.h build/gen/xdg-shell-protocol.c build/pc/liblua.a
	mkdir -p build/pc
	$(CC) $(CFLAGS) -o build/pc/hu $(PC_SRC) build/pc/liblua.a $(PC_LIB)

pc-asan: src/aa_cert.h build/gen/xdg-shell-client-protocol.h build/gen/xdg-shell-protocol.c build/asan/liblua.a
	mkdir -p build/pc
	$(CC) -std=gnu99 $(CWARN) -Isrc -Ibuild/gen -O1 -g -fno-omit-frame-pointer \
	  -fsanitize=address,undefined -o build/pc/hu-asan $(PC_SRC) build/asan/liblua.a $(PC_LIB)

arm: src/aa_cert.h build/arm/liblua.a
	mkdir -p build/arm
	$(ARM_CC) $(CFLAGS) -g -funwind-tables -I/usr/include -o build/arm/hu.debug $(SRC) src/platform_fb.c src/bt_nfore.c build/arm/liblua.a \
	  -L/sysroot/usr/lib -Wl,-rpath-link,/sysroot/usr/lib -lEGL -lGLESv2 -lbluelink -lpthread -ldl -lm
	arm-linux-gnueabihf-objcopy --strip-all build/arm/hu.debug build/arm/hu   # on the HU - no symbols; crash stack - addr2line against hu.debug
	arm-linux-gnueabihf-readelf -V build/arm/hu | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1

build/tools/bake_fonts: tools/bake_fonts.c
	mkdir -p build/tools
	$(CC) -O2 -o $@ $< -lm

assets: build/tools/bake_fonts
	mkdir -p assets/fonts
	cp assets_fonts.txt assets/fonts/fonts.txt
	build/tools/bake_fonts res/fonts assets/fonts 0.8
	python3 tools/make_icon_atlas.py assets
	cp res/bg_*.png res/app_*.png assets/
	mkdir -p assets/demo && cp res/demo/*.png assets/demo/

clean:
	rm -rf build

.PHONY: pc pc-asan arm assets clean
