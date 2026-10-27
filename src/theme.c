/* Theme palettes from the design (Main.dc.html, renderVals: c = dark ? {...} : {...}). */
#include "theme.h"

palette PAL;
int theme_accent;

const char *const ACCENT_NAME[N_ACCENT] = { "Blue", "Teal", "Green", "Orange", "Red", "Pink", "Purple" };
static const uint32_t ACC_D[N_ACCENT] = { 0x4C9BFF, 0x2CC9C0, 0x3DD68C, 0xFF9F43, 0xFF6B6B, 0xFF7EB6, 0xA78BFF };
static const uint32_t ACC_L[N_ACCENT] = { 0x2F7BF6, 0x0E8A84, 0x16834F, 0xC4580A, 0xD42F3A, 0xC7357A, 0x6C47E8 };
uint32_t accent_rgb(int k, int light) { return light ? ACC_L[k] : ACC_D[k]; }
void theme_set_accent(int k) { theme_accent = k; PAL.accent = hexc(accent_rgb(k, PAL.light)); }

void theme_set(int light)
{
    palette p;
    if (!light) {
        p.text = hexc(0xF5F5F7); p.sub = hexc(0xE5E5EA); p.muted = hexc(0xC7C7CC);
        p.border = hexa(0xFFFFFF, 0.12f); p.inner = hexa(0xFFFFFF, 0.10f); p.track = hexa(0xFFFFFF, 0.22f);
        p.inv = hexc(0xF5F5F7); p.oninv = hexc(0x000000); p.accent = hexc(0x4C9BFF); p.onaccent = hexc(0x000000); p.ok = hexc(0x2DBE6C);
        p.railact = hexa(0xFFFFFF, 0.16f); p.seg = hexa(0x000000, 0.35f); p.tick = hexa(0xFFFFFF, 0.35f);
        p.glass_tint = hexc(0x18181C); p.glass_a = 0.66f;     /* v35: rgba(24,24,28,.66), blur(48px) saturate(150%) */
        p.glass_blur = 48 / 30.0f; p.glass_sat = 1.5f;
        p.rail_tint = hexc(0x0A0A0C); p.rail_a = 0.38f;
        p.hud_tint = hexc(0x1E1E22); p.hud_a = 0.60f;
        p.glass_edge = 0.12f;
        p.panel_shadow = hexa(0x000000, 0.0f);
    } else {
        p.text = hexc(0x111114); p.sub = hexc(0x3A3A40); p.muted = hexc(0x55555C);
        p.border = hexa(0xFFFFFF, 0.75f); p.inner = hexa(0x000000, 0.06f); p.track = hexa(0x000000, 0.12f);
        p.inv = hexc(0x111114); p.oninv = hexc(0xFFFFFF); p.accent = hexc(0x2F7BF6); p.onaccent = hexc(0xFFFFFF); p.ok = hexc(0x13803F);
        p.railact = hexa(0x000000, 0.08f); p.seg = hexa(0x000000, 0.06f); p.tick = hexa(0x000000, 0.28f);
        p.glass_tint = hexc(0xFFFFFF); p.glass_a = 0.55f;
        p.glass_blur = 1; p.glass_sat = 1.6f;                  /* blur(30px) saturate(160%) */
        p.rail_tint = hexc(0xFFFFFF); p.rail_a = 0.50f;
        p.hud_tint = hexc(0xFFFFFF); p.hud_a = 0.75f;
        p.glass_edge = 0.75f;
        p.panel_shadow = hexa(0x141428, 0.10f);
    }
    p.light = light;
    p.accent = hexc(accent_rgb(theme_accent, light));
    PAL = p;
}
