/* Text: Figtree glyph atlases, pre-baked on the PC (tools/bake_fonts.c).
 * On the HU the CPU only reads the tables and uploads the atlas to video memory; the GPU draws (gfx_glyphs). */
#ifndef TEXT_H
#define TEXT_H

#include "gfx.h"

typedef struct font font;

int   text_init(const char *dir);           /* reads dir/fonts.txt and the atlases */
void  text_shutdown(void);
font *text_font(int weight, float design_px);   /* weight 500/600/700/800, size in design px */
float text_width(const font *f, const char *utf8, float letter_spacing);
float text_line_height(const font *f);          /* line-height: normal (as in CSS) */
float text_ascent(const font *f);
/* y - top of the line (like a CSS block); letter_spacing - in screen pixels */
void  text_draw(const font *f, float x, float y, const char *utf8, col c, float letter_spacing);
/* equal-width digits (font-variant-numeric: tabular-nums) for the following text_width / text_draw; 0 - off */
void  text_tnum(int on);

/* HTML codes in the string, in place (&#50668; &#x2014; &amp; &lt; &gt; &quot; &#39;) -> UTF-8: some apps send the track
 * like this (RadioWave - both in AA and over Bluetooth, 2026-10-26). The result is never longer than the source */
void text_unescape(char *s);

#endif
