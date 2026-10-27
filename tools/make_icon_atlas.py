#!/usr/bin/env python3
"""Icon atlas for the renderer: all design icons in white, one 96x96 cell each in a 1024x1024 texture (POT -> mipmaps).
Color is set at draw time (the shader multiplies). Icon paths are taken from tools/make_assets.py (single source).
Run (in the tiggo-pc container, rsvg-convert required): python3 hu/tools/make_icon_atlas.py <out_dir>"""
import importlib.util, os, subprocess, sys, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ma', os.path.join(HERE, 'make_assets.py'))
ma = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ma)

CELL, SIZE = 96, 1024
# line thickness variants: (name, thickness) - None = as in the design
VARIANTS = [(n, None) for n in ma.ICONS] + [('music', 2.4), ('video', 2.4), ('radio', 2.4), ('camera', 2.4), ('music', 1.3),
            ('video', 1.0), ('camera', 1.2), ('phone', 2.2), ('bluetooth', 2.2), ('sliders', 2.2), ('photo', 2.2),
            ('app_display', 2.4), ('app_video', 2.4), ('bluetooth', 2.1), ('car', 1.8), ('app_usb', 1.9),
            ('app_music', 1.9), ('app_video', 1.9), ('app_display', 1.9), ('bin', 1.8), ('bin', 2.0),
            ('chev_r', 2.4), ('chev_l', 2.4), ('check', 2.6), ('plus', 2.0), ('close', 2.2),
            ('app_music', 2.2), ('video', 0.8), ('fullscreen', 2.2), ('chev_l', 2.2),
            ('power', None), ('moon2', None), ('sun', 2.0),
            ('heart', None), ('heart_o', None), ('scan', None), ('waves', None), ('nosignal', None), ('power', 2.2), ('chev_r', 2.2),
            ('globe', 2.1), ('globe', 2.2), ('bluetooth', 2.0), ('usb_full', 2.2), ('usb_full', 1.8), ('device', 2.2),
            ('app_lua', 2.0), ('speaker', 2.2), ('video', 1.8), ('app_usb', 2.2),
            ('app_video', 2.2), ('app_radio', 2.2), ('app_camera', 2.2), ('app_phone', 2.2)]

out = sys.argv[1]
atlas = Image.new('RGBA', (SIZE, SIZE * 2), (255, 255, 255, 0))   # 10 x 21 cells
lines = []
per_row = SIZE // CELL
with tempfile.TemporaryDirectory() as tmp:
    for i, (name, sw_over) in enumerate(VARIANTS):
        inner, sw = ma.ICONS[name]
        if sw_over is not None:
            sw = sw_over
        attrs = 'fill="#FFFFFF"' if sw is None else \
            'fill="none" stroke="#FFFFFF" stroke-width="%s" stroke-linecap="round" stroke-linejoin="round"' % sw
        key = name + ('' if sw_over is None else '_w%s' % str(sw_over).replace('.', ''))
        svg = os.path.join(tmp, key + '.svg')
        png = os.path.join(tmp, key + '.png')
        with open(svg, 'w') as f:
            f.write('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" %s>%s</svg>' % (attrs, inner))
        subprocess.check_call(['rsvg-convert', '-w', str(CELL), '-h', str(CELL), '-o', png, svg])
        x, y = (i % per_row) * CELL, (i // per_row) * CELL
        atlas.paste(Image.open(png).convert('RGBA'), (x, y))
        lines.append('%s %d %d %d %d' % (key, x, y, CELL, CELL))
# PNG masks from the design (icons_png/, see tools/extract_design_png.py): take the alpha, the icon is white (tinted in the shader)
n = len(VARIANTS)
for f in sorted(os.listdir('icons_png')):
    if not f.endswith('.png'):
        continue
    m = Image.open(os.path.join('icons_png', f)).convert('RGBA').split()[3].resize((CELL, CELL), Image.LANCZOS)
    im = Image.new('RGBA', (CELL, CELL), (255, 255, 255, 0))
    im.putalpha(m)
    x, y = (n % per_row) * CELL, (n // per_row) * CELL
    atlas.paste(im, (x, y))
    lines.append('png_%s %d %d %d %d' % (f[:-4], x, y, CELL, CELL))
    n += 1
atlas.save(os.path.join(out, 'icons.png'))
with open(os.path.join(out, 'icons.txt'), 'w') as f:
    f.write('\n'.join(lines) + '\n')
print('icons:', len(lines))
