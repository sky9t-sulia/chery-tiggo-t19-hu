/* Version of our shell. Bump on every notable build/upload.
 * The full string (with compile date/time) is visible: in the startup log, via `strings hu | grep hu-build`
 * and in the file HU_DIR/version on the HU (written at startup). */
#ifndef HU_VERSION_H
#define HU_VERSION_H

#define HU_VERSION "0.9.4"
#define HU_NAME "CheryTiggo4"      /* HU name: Bluetooth, Android Auto, "This device" (owner 2026-10-26; "Car Media" in the design) */
#define HU_BUILD_ID ("hu-build " HU_VERSION " " __DATE__ " " __TIME__)

#endif
