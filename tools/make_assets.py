#!/usr/bin/env python3
"""Bakes the UI assets from the Car Media design (design/project/Main.dc.html):
- scene background (radial gradients + dark-theme overlay) into a 1024x600 PNG;
- SVG icons (paths from the design, viewBox 24) with the color baked in.
Run: python3 hu/tools/make_assets.py  -> hu/res/
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'res')
OUT_W, OUT_H = 1024, 600
K = 0.8                      # the design is laid out at 1280x750 and scaled by 0.8

# scenes from the design: (rx, ry, X%, Y%, color, where transparent) ..., base
SCENES = {
    'dusk': ([(900, 600, 12, 95, '#ff6a3d', .60), (800, 620, 88, 8, '#5b3cff', .60),
              (700, 520, 58, 72, '#c2185b', .60)], '#120a1e'),
    'ocean': ([(900, 640, 10, 18, '#0077b6', .60), (820, 620, 92, 92, '#00b4a0', .60),
               (600, 420, 62, 30, '#023e8a', .65)], '#020c1b'),
    'aurora': ([(1000, 500, 30, 6, '#2ee59d', .55), (820, 620, 86, 42, '#7b2ff7', .60),
                (700, 520, 18, 100, '#0f4c81', .60)], '#050814'),
    'forest': ([(900, 620, 18, 30, '#2d6a4f', .60), (820, 600, 86, 88, '#95a85a', .55),
                (620, 520, 72, 18, '#1b4332', .60)], '#07120c'),
    'graphite': ([(900, 620, 18, 18, '#3a3d44', .60), (820, 620, 92, 92, '#2a2d34', .60)], '#0b0c0e'),
    # v26
    'sunset': ([(900, 600, 50, 105, '#ff9e3d', .60), (900, 600, 50, -10, '#e0457b', .60), (600, 500, 90, 50, '#6a1b9a', .60)], '#1a0b16'),
    'lagoon': ([(900, 620, 85, 15, '#00c2d1', .58), (820, 620, 12, 88, '#1565c0', .60), (500, 400, 50, 50, '#26a69a', .65)], '#03121a'),
    'midnight': ([(900, 620, 20, 20, '#1e2a78', .60), (820, 620, 85, 85, '#3949ab', .55), (500, 380, 70, 25, '#0d1440', .60)], '#04050f'),
    'ember': ([(900, 620, 15, 85, '#d84315', .58), (820, 620, 85, 15, '#8e1b1b', .60), (600, 460, 60, 60, '#ff7043', .62)], '#140605'),
    'rose': ([(900, 620, 20, 15, '#f06292', .58), (820, 620, 88, 88, '#8e24aa', .60), (600, 460, 70, 30, '#ff8a80', .62)], '#16070f'),
    'sand': ([(900, 620, 20, 80, '#c8a46b', .58), (820, 620, 85, 20, '#8d6e63', .60), (600, 460, 55, 45, '#e0b98a', .62)], '#15100a'),
}
OVERLAY_DARK = (0, 0, 0, 0.28)
OVERLAY_LIGHT = (1, 1, 1, 0.48)


def rgb(h):
    h = h.lstrip('#')
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def bake_scene(name, overlay=OVERLAY_DARK, suffix=''):
    layers, base = SCENES[name]
    # scene layer in the design: inset -40px from 1280x750 -> 1360x830, origin at (-40,-40)
    bw, bh = 1360.0, 830.0
    ys, xs = np.mgrid[0:OUT_H, 0:OUT_W].astype(np.float64)
    dx_design = xs / K + 40.0           # coordinates inside the scene layer
    dy_design = ys / K + 40.0
    img = np.ones((OUT_H, OUT_W, 3)) * rgb(base)
    for rx, ry, px, py, col, stop in reversed(layers):   # first in the CSS list - on top
        cx, cy = bw * px / 100.0, bh * py / 100.0
        t = np.sqrt(((dx_design - cx) / rx) ** 2 + ((dy_design - cy) / ry) ** 2)
        a = np.clip(1.0 - t / stop, 0.0, 1.0)[..., None]
        img = img * (1 - a) + rgb(col) * a
    r, g, b, a = overlay
    img = img * (1 - a) + np.array([r, g, b]) * a
    img += (np.random.default_rng(1).random(img.shape) - 0.5) / 255.0   # dithering against banding
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(
        os.path.join(ROOT, 'bg_%s%s.png' % (name, suffix)), optimize=True)


def bake_thumb(name):
    # scene tile on the Display screen (v26: 6x2, 170x98 design px): in the design the tile background is the same scene CSS gradients
    # (radii in px, centers in % of the tile), so only part of them is visible. Without the theme darkening. 2x of the screen.
    layers, base = SCENES[name]
    tw, th, ow, oh = 170.0, 98.0, 272, 157
    ys, xs = np.mgrid[0:oh, 0:ow].astype(np.float64)
    dx, dy = (xs + 0.5) * tw / ow, (ys + 0.5) * th / oh
    img = np.ones((oh, ow, 3)) * rgb(base)
    for rx, ry, px, py, col, stop in reversed(layers):
        t = np.sqrt(((dx - tw * px / 100.0) / rx) ** 2 + ((dy - th * py / 100.0) / ry) ** 2)
        a = np.clip(1.0 - t / stop, 0.0, 1.0)[..., None]
        img = img * (1 - a) + rgb(col) * a
    img += (np.random.default_rng(1).random(img.shape) - 0.5) / 255.0
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(
        os.path.join(ROOT, 'bg_%s_thumb.png' % name), optimize=True)


# --- demo photos from the design (this.IMG): CSS linear/radial gradients with hard stops -> 1024x600 PNG
def css_color(c):
    c = c.strip()
    if c.startswith('#'):
        return np.array(list(rgb(c)) + [1.0])
    if c.startswith('rgba'):
        v = [float(x) for x in c[5:-1].split(',')]
        return np.array([v[0] / 255, v[1] / 255, v[2] / 255, v[3]])
    raise ValueError(c)

def stops_eval(stops, t):
    # stops: [(color rgba, pos 0..1)], t: array; linear interpolation in premultiplied
    out = np.zeros(t.shape + (4,))
    cols = [np.array([c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]]) for c, _ in stops]
    pos = [p for _, p in stops]
    out[:] = cols[0]
    for k in range(len(stops) - 1):
        a, b = pos[k], pos[k + 1]
        m = (t >= a) & (t <= b)
        f = np.clip((t - a) / max(b - a, 1e-6), 0, 1)[..., None]
        out[m] = (cols[k] * (1 - f) + cols[k + 1] * f)[m]
    out[t > pos[-1]] = cols[-1]
    return out

def layer_linear(deg, stops, W, H):
    a = np.radians(deg)
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64) + 0.5
    dx, dy = np.sin(a), -np.cos(a)                    # CSS: 0deg is up, clockwise
    L = abs(W * np.sin(a)) + abs(H * np.cos(a))
    t = ((xs - W / 2) * dx + (ys - H / 2) * dy) / L + 0.5
    return stops_eval(stops, t)

def layer_radial(cx, cy, stops, W, H):
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64) + 0.5
    px, py = W * cx, H * cy
    # circle, farthest-corner
    R = max(np.hypot(px - x, py - y) for x in (0, W) for y in (0, H))
    t = np.hypot(xs - px, ys - py) / R
    return stops_eval(stops, t)

def parse_stops(parts):
    st = []
    for p in parts:
        p = p.strip()
        if p.endswith('%') and (' ' in p):
            col, pc = p.rsplit(' ', 1)
            st.append((css_color(col), float(pc[:-1]) / 100))
        else:
            st.append((css_color(p), None))
    n = len(st)
    for k, (c, pp) in enumerate(st):
        if pp is None:
            st[k] = (c, 0.0 if k == 0 else 1.0 if k == n - 1 else k / (n - 1))
    return st

def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out]

def render_css_bg(css, W, H):
    img = np.zeros((H, W, 4))
    for layer in reversed(split_top(css)):            # first CSS layer - on top
        if layer.startswith('linear-gradient('):
            parts = split_top(layer[len('linear-gradient('):-1])
            if parts[0].endswith('deg'):
                deg = float(parts[0][:-3]); parts = parts[1:]
            else:
                deg = 180.0
            L = layer_linear(deg, parse_stops(parts), W, H)
        elif layer.startswith('radial-gradient('):
            parts = split_top(layer[len('radial-gradient('):-1])
            pos = parts[0].split(' at ')[1].split()
            L = layer_radial(float(pos[0][:-1]) / 100, float(pos[1][:-1]) / 100, parse_stops(parts[1:]), W, H)
        else:
            c = css_color(layer); L = np.zeros((H, W, 4)); L[:] = [c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]]
        img = L + img * (1 - L[..., 3:4])
    rgbv = img[..., :3] / np.maximum(img[..., 3:4], 1e-6) * (img[..., 3:4] > 0)
    rgbv += (np.random.default_rng(3).random(rgbv.shape) - 0.5) / 255.0
    return Image.fromarray((np.clip(rgbv, 0, 1) * 255 + 0.5).astype(np.uint8))

DEMO_IMG = [
    ('IMG_0412', 'radial-gradient(circle at 50% 56%, #ffe7b0 0%, #ffd27a 6%, rgba(255,210,122,0) 7%), linear-gradient(180deg, #ffb36b 0%, #ff7a6b 38%, #c2456e 55%, #2b1840 56%, #120a1e 100%)'),
    ('IMG_0418', 'linear-gradient(160deg, rgba(0,0,0,0) 55%, #1e3a2f 55.3%), linear-gradient(200deg, rgba(0,0,0,0) 48%, #2c5240 48.3%), linear-gradient(180deg, #9fd3ff 0%, #e6f4ff 62%)'),
    ('IMG_0433', 'radial-gradient(circle at 70% 30%, #fffbe6 0%, #fff1b8 4%, rgba(255,241,184,0) 5%), linear-gradient(180deg, #7cc6ff 0%, #bfe6ff 48%, #1d6fa3 49%, #0b3a5c 100%)'),
    ('IMG_0457', 'radial-gradient(circle at 78% 22%, #fff7d6 0%, #fff7d6 3%, rgba(255,247,214,0) 3.6%), linear-gradient(180deg, #0b1026 0%, #26215a 70%, #3d2a6b 100%)'),
    ('IMG_0461', 'linear-gradient(170deg, rgba(0,0,0,0) 60%, #c97b3a 60.3%), linear-gradient(190deg, rgba(0,0,0,0) 52%, #e3a35a 52.3%), linear-gradient(180deg, #ffd9a0 0%, #ffeccd 58%)'),
    ('IMG_0479', 'linear-gradient(180deg, rgba(255,255,255,0.35) 0%, rgba(255,255,255,0) 45%), linear-gradient(180deg, #c9d6cf 0%, #8fa89a 50%, #2f4a3c 100%)'),
    ('IMG_0502', 'radial-gradient(circle at 20% 30%, #ff3d9a 0%, rgba(255,61,154,0) 45%), radial-gradient(circle at 80% 70%, #3df5ff 0%, rgba(61,245,255,0) 45%), #10061f'),
    ('IMG_0517', 'linear-gradient(150deg, rgba(0,0,0,0) 50%, #e9f1f7 50.3%, #b9cbdc 72%), linear-gradient(210deg, rgba(0,0,0,0) 44%, #f5f9fc 44.3%, #c9d8e6 70%), linear-gradient(180deg, #4d7fb5 0%, #a8c8e8 60%)'),
]

def bake_demo_images():
    os.makedirs(os.path.join(ROOT, 'demo'), exist_ok=True)
    for name, css in DEMO_IMG:
        render_css_bg(css, 1024, 600).save(os.path.join(ROOT, 'demo', name + '.png'), optimize=True)


# --- icons: name -> (svg contents, stroke-width or None for fill) ---
ICONS = {
    'music': ('<path d="M9 18V5l11-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/>', 2),
    'video': ('<rect x="3" y="3" width="18" height="18" rx="3"/><path d="M7.5 3v18M16.5 3v18M3 8h4.5M3 12h4.5M3 16h4.5M16.5 8H21M16.5 12H21M16.5 16H21"/>', 2),
    'radio': ('<rect x="3" y="8" width="18" height="13" rx="3"/><path d="M7 8l9-5M6.5 12.5h3M6.5 16.5h3"/><circle cx="15.5" cy="14.5" r="3"/>', 2),
    'camera': ('<path d="M3 8a2 2 0 0 1 2-2h2.5L9 4h6l1.5 2H19a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><circle cx="12" cy="13" r="3.5"/>', 2),
    'phone': ('<path d="M5 4h4l2 5-2.5 1.5a11 11 0 0 0 5 5L15 13l5 2v4a2 2 0 0 1-2 2A16 16 0 0 1 3 6a2 2 0 0 1 2-2"/>', 2),
    'apps': ('<circle cx="6" cy="6" r="2"/><circle cx="12" cy="6" r="2"/><circle cx="18" cy="6" r="2"/><circle cx="6" cy="12" r="2"/><circle cx="12" cy="12" r="2"/><circle cx="18" cy="12" r="2"/><circle cx="6" cy="18" r="2"/><circle cx="12" cy="18" r="2"/><circle cx="18" cy="18" r="2"/>', None),
    'dashboard': ('<rect x="3" y="3" width="10" height="18" rx="2.5"/><rect x="15" y="3" width="6" height="8" rx="2"/><rect x="15" y="13" width="6" height="8" rx="2"/>', 2),
    'signal': ('<path d="M4 20v-3M9 20v-7M14 20V9M19 20V4"/>', 2.4),
    'bluetooth': ('<path d="M7 7l10 10-5 5V2l5 5L7 17"/>', 2.4),
    'prev': ('<path d="M6 5h2v14H6zM20 5v14L9 12z"/>', None),
    'next': ('<path d="M16 5h2v14h-2zM4 5v14l11-7z"/>', None),
    'pause': ('<path d="M6 4h4v16H6zM14 4h4v16h-4z"/>', None),
    'play': ('<path d="M7 4l13 8-13 8z"/>', None),
    'ac': ('<path d="M12 2v20M3.5 7l17 10M20.5 7l-17 10"/>', 2.2),
    'flame': ('<path d="M12 22c3.9 0 7-2.9 7-7 0-3-2-5.2-3-7-1 2-2 3-3 3 0-3-1-6-4-9 0 4-4 7-4 13 0 4.1 3.1 7 7 7z"/>', 2),
    'minus': ('<path d="M5 12h14"/>', 2.4),
    'plus': ('<path d="M5 12h14M12 5v14"/>', 2.4),
    'fan': ('<circle cx="12" cy="12" r="1.6"/><path d="M12 10.4c0-4 1.2-7 3.6-7s2 4-3.6 7zM13.6 12c4 0 7 1.2 7 3.6s-4 2-7-3.6zM12 13.6c0 4-1.2 7-3.6 7s-2-4 3.6-7zM10.4 12c-4 0-7-1.2-7-3.6s4-2 7 3.6z"/>', 1.9),
    'air_face': ('<circle cx="7" cy="4.5" r="2"/><path d="M7 8v6h5l1.5 7"/><path d="M22 8h-8M17 5l-3 3 3 3"/>', 1.9),
    'air_facefeet': ('<circle cx="7" cy="4.5" r="2"/><path d="M7 8v6h5l1.5 7"/><path d="M22 8h-7M18 5.5L15 8l3 2.5M22 19h-5M20 16.5L17 19l3 2.5"/>', 1.9),
    'air_feet': ('<circle cx="7" cy="4.5" r="2"/><path d="M7 8v6h5l1.5 7"/><path d="M22 19h-6M19 16l-3 3 3 3"/>', 1.9),
    'air_feetwind': ('<circle cx="7" cy="4.5" r="2"/><path d="M7 8v6h5l1.5 7"/><path d="M22 19h-6M19 16l-3 3 3 3"/><path d="M15 2.5c1 1 1 2 0 3M18.5 2.5c1 1 1 2 0 3M22 2.5c1 1 1 2 0 3"/>', 1.9),
    'air_wind': ('<path d="M3 20L8 5h13"/><path d="M12 17c1.2-1.2 1.2-2.3 0-3.5s-1.2-2.3 0-3.5M16 17c1.2-1.2 1.2-2.3 0-3.5s-1.2-2.3 0-3.5"/>', 1.9),
    'sun': ('<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>', 2.2),
    'vol_down': ('<path d="M11 5L6 9H3v6h3l5 4z"/><path d="M15.5 12h4"/>', 2),
    'vol_up': ('<path d="M11 5L6 9H3v6h3l5 4z"/><path d="M15.5 12h5M18 9.5v5"/>', 2),
    'shuffle': ('<path d="M16 3h5v5M4 20L21 3M21 16v5h-5M15 15l6 6M4 4l5 5"/>', 2),
    'repeat': ('<path d="M17 2l4 4-4 4"/><path d="M3 11V9a3 3 0 0 1 3-3h15M7 22l-4-4 4-4"/><path d="M21 13v2a3 3 0 0 1-3 3H3"/>', 2),
    'back10': ('<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/>', 2),
    'fwd10': ('<path d="M21 12a9 9 0 1 1-3-6.7L21 8"/><path d="M21 3v5h-5"/>', 2),
    'subtitles': ('<rect x="2" y="5" width="20" height="14" rx="3"/><path d="M7 12h3M14 12h3M7 15.5h10"/>', 2),
    'heart': ('<path d="M12 20.5s-7.6-4.6-9.5-9.4C1.2 7.8 3.4 4.5 6.8 4.5c2.1 0 3.6 1.1 5.2 3 1.6-1.9 3.1-3 5.2-3 3.4 0 5.6 3.3 4.3 6.6-1.9 4.8-9.5 9.4-9.5 9.4z"/>', None),
    'heart_o': ('<path d="M12 20.5s-7.6-4.6-9.5-9.4C1.2 7.8 3.4 4.5 6.8 4.5c2.1 0 3.6 1.1 5.2 3 1.6-1.9 3.1-3 5.2-3 3.4 0 5.6 3.3 4.3 6.6-1.9 4.8-9.5 9.4-9.5 9.4z"/>', 2.1),
    'scan': ('<path d="M20 12a8 8 0 1 1-2.3-5.7M20 4v4.5h-4.5"/>', 2.2),
    'waves': ('<path d="M5 19a10 10 0 0 1 0-14M19 5a10 10 0 0 1 0 14M8.5 15.5a5 5 0 0 1 0-7M15.5 8.5a5 5 0 0 1 0 7"/><circle cx="12" cy="12" r="1.6"/>', 2.1),
    'nosignal': ('<path d="M5 19a10 10 0 0 1 0-14M19 5a10 10 0 0 1 0 14M3 3l18 18"/>', 2.1),
    'power': ('<path d="M12 3v8M6.3 6.3a8 8 0 1 0 11.4 0"/>', 2.4),
    'wake': ('<path d="M12 3v8M6.4 6.6a8 8 0 1 0 11.2 0"/>', 2),   # Screen off: button (v28)
    'moon2': ('<path d="M20.5 14.5A8.5 8.5 0 0 1 9.5 3.5a8.5 8.5 0 1 0 11 11z"/>', 2),
    'pl_bars': ('<path d="M6 18v-6M10 18V8M14 18v-9M18 18v-5"/>', 2.2),
    'fs_exit': ('<path d="M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5"/>', 2),
    'fullscreen': ('<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>', 2),
    'chev_l': ('<path d="M15 5l-7 7 7 7"/>', 2.2),
    'chev_r': ('<path d="M9 5l7 7-7 7"/>', 2.2),
    'check': ('<path d="M5 12l5 5 9-10"/>', 3),
    'globe': ('<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c2.5 2.6 3.8 5.6 3.8 9s-1.3 6.4-3.8 9c-2.5-2.6-3.8-5.6-3.8-9S9.5 5.6 12 3z"/>', 2),
    'sig1': ('<path d="M4 20v-3"/>', 2.4),
    'sig2': ('<path d="M9 20v-7"/>', 2.4),
    'sig3': ('<path d="M14 20V9"/>', 2.4),
    'sig4': ('<path d="M19 20V4"/>', 2.4),
    'eject': ('<path d="M12 4l8 9H4zM4 18h16"/>', 2.2),
    'globe_off': ('<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c2.5 2.6 3.8 5.6 3.8 9s-1.3 6.4-3.8 9c-2.5-2.6-3.8-5.6-3.8-9S9.5 5.6 12 3zM3 3l18 18"/>', 2.2),
    'chev_d': ('<path d="M6 9l6 6 6-6"/>', 2.4),
    'chev_u': ('<path d="M6 15l6-6 6 6"/>', 2.4),
    'tick': ('<path d="M5 12.5l4.5 4.5L19 7.5"/>', 2.6),
    'photo': ('<rect x="3" y="3" width="18" height="18" rx="3"/><circle cx="9" cy="9" r="2"/><path d="M21 16l-5-5-9 10"/>', 2),
    'moon': ('<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>', 2.2),
    'auto_theme': ('<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 1 0 18z" fill="#FFFFFF"/>', 2.2),   # v30: Auto theme
    'backspace': ('<path d="M9 5h11a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H9l-6-7z"/><path d="M12 9.5l5 5M17 9.5l-5 5"/>', 2),
    'mic_off': ('<path d="M9 9v3a3 3 0 0 0 5 2.2M15 9.3V5a3 3 0 0 0-5.7-1.3M19 11a7 7 0 0 1-1.2 4M5 11a7 7 0 0 0 11 5.7M12 18v3M3 3l18 18"/>', 2),
    'speaker': ('<path d="M11 5L6 9H3v6h3l5 4z"/><path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"/>', 2),
    'usb': ('<path d="M12 2v16M9 5l3-3 3 3M7 10v3l5 3M17 8v3l-5 3"/><circle cx="12" cy="20" r="2"/>', 2.2),
    'guides': ('<path d="M6 21L10 3M18 21L14 3M7.5 15h3M16.5 15h-3"/>', 2),
    'device': ('<rect x="6" y="2" width="12" height="20" rx="3"/><path d="M11 18h2"/>', 2),
    'sliders': ('<path d="M6 4v8M6 16v4M12 4v2M12 10v10M18 4v10M18 18v2M4 14h4M10 8h4M16 16h4"/>', 2.2),
    'app_music': ('<path d="M9 18V5l11-2v13M6 21a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM17 19a3 3 0 1 0 0-6 3 3 0 0 0 0 6z"/>', 1.8),
    'app_video': ('<path d="M6 3h12a3 3 0 0 1 3 3v12a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3V6a3 3 0 0 1 3-3zM7.5 3v18M16.5 3v18M3 8h4.5M3 12h4.5M3 16h4.5M16.5 8H21M16.5 12H21M16.5 16H21"/>', 1.8),
    'app_radio': ('<path d="M6 8h12a3 3 0 0 1 3 3v7a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3v-7a3 3 0 0 1 3-3zM7 8l9-5M6.5 12.5h3M6.5 16.5h3M15.5 17.5a3 3 0 1 0 0-6 3 3 0 0 0 0 6z"/>', 1.8),
    'app_camera': ('<path d="M3 8a2 2 0 0 1 2-2h2.5L9 4h6l1.5 2H19a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2zM12 16.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7z"/>', 1.8),
    'app_phone': ('<path d="M5 4h4l2 5-2.5 1.5a11 11 0 0 0 5 5L15 13l5 2v4a2 2 0 0 1-2 2A16 16 0 0 1 3 6a2 2 0 0 1 2-2"/>', 1.8),
    'app_usb': ('<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>', 1.8),
    'app_lua': ('<path d="M11 21a8 8 0 1 0 0-16 8 8 0 0 0 0 16zM12.5 13.5a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM19 7a2 2 0 1 0 0-4 2 2 0 0 0 0 4z"/>', 1.8),
    'app_bt': ('<path d="M7 7l10 10-5 5V2l5 5L7 17"/>', 1.8),
    'app_sound': ('<path d="M6 4v8M6 16v4M12 4v2M12 10v10M18 4v10M18 18v2M4 14h4M10 8h4M16 16h4"/>', 1.8),
    'app_display': ('<path d="M6 3h12a3 3 0 0 1 3 3v12a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3V6a3 3 0 0 1 3-3zM9 11a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM21 16l-5-5-9 10"/>', 1.8),
    'app_settings': ('<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2zM12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z"/>', 1.8),
    'close': ('<path d="M6 6l12 12M18 6L6 18"/>', 2.4),
    'search': ('<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>', 2.2),
    'trash': ('<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>', 2.2),
    'spinner': ('<path d="M12 3a9 9 0 1 1-9 9"/>', 2.6),
    'swap': ('<path d="M4 8h13l-3-3M20 16H7l3 3"/>', 2.2),
    'conn': ('<path d="M9 7V3M15 7V3M6 7h12v4a6 6 0 0 1-12 0zM12 17v4"/>', 2.4),
    'usb_full': ('<path d="M12 2v16M9 5l3-3 3 3M7 10v3l5 3M17 8v3l-5 3M12 22a2 2 0 1 0 0-4 2 2 0 0 0 0 4z"/>', 2.1),
    'car': ('<path d="M5 11l1.5-4.5A2 2 0 0 1 8.4 5h7.2a2 2 0 0 1 1.9 1.5L19 11M4 11h16v6a1 1 0 0 1-1 1h-1a1 1 0 0 1-1-1v-1H7v1a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1zM7.5 14h.01M16.5 14h.01"/>', 2),
    'gauge': ('<path d="M4 16a8 8 0 1 1 16 0M12 16l3.5-4.5M7 16h.01M17 16h.01"/>', 2.2),
    'clock': ('<path d="M12 22a10 10 0 1 1 0-20 10 10 0 0 1 0 20zM12 6v6l4 2"/>', 2.2),
    'vol_media': ('<path d="M11 5L6 9H3v6h3l5 4zM15.5 8.5a5 5 0 0 1 0 7"/>', 2.2),
    'vol_muted': ('<path d="M11 5L6 9H3v6h3l5 4zM16 9l5 6M21 9l-5 6"/>', 2.2),
    'bt_phone': ('<path d="M9 2h6a3 3 0 0 1 3 3v14a3 3 0 0 1-3 3H9a3 3 0 0 1-3-3V5a3 3 0 0 1 3-3zM11 18h2"/>', 2),
    'bt_tablet': ('<path d="M7 2h10a3 3 0 0 1 3 3v14a3 3 0 0 1-3 3H7a3 3 0 0 1-3-3V5a3 3 0 0 1 3-3zM11 18h2"/>', 2),
    'bt_speaker': ('<path d="M8 2h8a3 3 0 0 1 3 3v14a3 3 0 0 1-3 3H8a3 3 0 0 1-3-3V5a3 3 0 0 1 3-3zM12 18a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM12 7h.01"/>', 2),
    'laptop': ('<path d="M5 5h14a1 1 0 0 1 1 1v9H4V6a1 1 0 0 1 1-1zM2 19h20"/>', 2),
    'bin': ('<path d="M6 4h12a3 3 0 0 1 3 3v10a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3V7a3 3 0 0 1 3-3zM7 9l3 3-3 3M12 15h5"/>', 1.9),
    'doc': ('<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8zM14 3v5h5M9 13h6M9 17h6"/>', 1.9),
}

# (icon, color, thickness override) -> file icon_<name>_<color>.svg
VARIANTS = [
    ('music', 'FFFFFF', None), ('video', 'FFFFFF', None), ('radio', 'FFFFFF', None),
    ('camera', 'FFFFFF', None), ('phone', 'FFFFFF', None),
    ('apps', 'F5F5F7', None), ('dashboard', 'F5F5F7', None),
    ('signal', 'E5E5EA', None), ('bluetooth', 'E5E5EA', None),
    ('music', 'FFFFFF', 2.4), ('video', 'FFFFFF', 2.4), ('radio', 'FFFFFF', 2.4),   # small ones in cards
    ('music', 'FFFFFF', 1.3),                                                         # cover
    ('prev', 'F5F5F7', None), ('next', 'F5F5F7', None), ('pause', '000000', None), ('play', 'F5F5F7', None), ('play', '000000', None),
    ('ac', '000000', None), ('ac', '3B82F6', None), ('flame', 'EF4444', None),
    ('minus', 'F5F5F7', None), ('plus', 'F5F5F7', None), ('fan', 'F5F5F7', None),
] + [(a, c, None) for a in ('air_face', 'air_facefeet', 'air_feet', 'air_feetwind', 'air_wind')
      for c in ('000000', 'F5F5F7')] + [
    ('sun', 'F5F5F7', None), ('vol_down', 'F5F5F7', None), ('vol_up', 'F5F5F7', None),
]


def write_icon(name, color, sw_override):
    inner, sw = ICONS[name]
    if sw_override is not None:
        sw = sw_override
    if sw is None:
        attrs = 'fill="#%s"' % color
    else:
        attrs = ('fill="none" stroke="#%s" stroke-width="%s" stroke-linecap="round" '
                 'stroke-linejoin="round"' % (color, sw))
    suffix = '' if sw_override is None else '_w%s' % str(sw_override).replace('.', '')
    fn = os.path.join(ROOT, 'icons', 'icon_%s_%s%s.svg' % (name, color, suffix))
    with open(fn, 'w') as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" %s>%s</svg>\n'
                % (attrs, inner))


if __name__ == '__main__':
    os.makedirs(os.path.join(ROOT, 'icons'), exist_ok=True)
    for sc in SCENES:
        bake_scene(sc)
        bake_scene(sc, OVERLAY_LIGHT, '_light')
        bake_thumb(sc)
    for v in VARIANTS:
        write_icon(*v)
    bake_demo_images()
    print('ok:', sorted(os.listdir(ROOT)), len(os.listdir(os.path.join(ROOT, 'icons'))), 'icons')
