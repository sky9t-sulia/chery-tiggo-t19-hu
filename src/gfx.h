/* Graphics core: everything is drawn by OpenGL ES 2 shaders on the GPU.
 * Coordinates are screen pixels (0,0 is the top-left corner). Colors are plain (not premultiplied).
 * All GPU resources are created in gfx_init/gfx_tex_load and released in gfx_shutdown/gfx_tex_free;
 * no memory is allocated during a frame. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

typedef struct { float r, g, b, a; } col;

static inline col hexa(uint32_t rgb, float a)
{
    col c = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a };
    return c;
}
static inline col hexc(uint32_t rgb) { return hexa(rgb, 1.0f); }
static inline col col_alpha(col c, float a) { c.a *= a; return c; }
static inline col col_mix(col a, col b, float t)
{
    col c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    return c;
}

typedef struct { unsigned id; int w, h; } gfx_tex;

int  gfx_init(int screen_w, int screen_h);
void gfx_shutdown(void);
void gfx_frame_begin(void);
void gfx_finish(void);
int  gfx_tex_snapshot(gfx_tex *dst, const gfx_tex *src, float u1, float v1);   /* frame copy (RGBA, screen size) */                    /* wait for the GPU to finish drawing (before giving up the frame memory) */
void gfx_frame_end(void);

/* Shapes (SDF in the shader, smoothed edges) */
void gfx_rect(float x, float y, float w, float h, float radius, col fill);
void gfx_squircle(int on);           /* 1: from now on the corners of rect / shadow / glass / image_round are squircles (CSS corner-shape) */
void gfx_rect_border(float x, float y, float w, float h, float radius, col fill, float bw, col border);
/* Soft shadow / glow like CSS box-shadow (blur is the CSS blur radius) */
void gfx_shadow(float x, float y, float w, float h, float radius, float blur, float ox, float oy, col c);
/* Glass: blurred background under the panel + saturation + tint + border */
void gfx_glass(float x, float y, float w, float h, float radius, col tint, float tint_a, float edge_a);

/* Textures (PNG, premultiplied at load; mipmaps for POT textures) */
int  gfx_tex_load(gfx_tex *t, const char *path, int mipmaps);
void gfx_tex_free(gfx_tex *t);
int  gfx_tex_rgba(gfx_tex *t, int w, int h, const unsigned char *px);   /* from memory (RGBA) */
int  gfx_tex_yuv(gfx_tex *t, int w, int h, const void *vaddr, unsigned paddr);   /* NV12 video frame (VPU), zero-copy */
int  gfx_tex_yuyv(gfx_tex *t, int w, int h, const void *vaddr, unsigned paddr);  /* YUYV camera frame (V4L2), zero-copy */
void gfx_tex_yuv_dirty(gfx_tex *t);   /* a new frame has been written to the bound buffer */
void gfx_vgrad(float x, float y, float w, float h, col c, int top_opaque);   /* linear-gradient down/up */
void gfx_vgrad_round(float x, float y, float w, float h, col c, int top_opaque, float radius);
int  gfx_tex_alpha(gfx_tex *t, int w, int h);                     /* empty GL_ALPHA */
void gfx_tex_alpha_sub(gfx_tex *t, int x, int y, int w, int h, const unsigned char *px);
/* load with a color overlay (theme dimming on photo wallpapers) */
int  gfx_tex_load_overlay(gfx_tex *t, const char *path, col overlay);
/* Image / icon: tint is multiplied by the texture color (white icons can be colored any color) */
void gfx_image(const gfx_tex *t, float x, float y, float w, float h,
               float u0, float v0, float u1, float v1, col tint);
/* Image with rounded corners (radius in screen px) */
void gfx_image_round(const gfx_tex *t, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, col tint, float radius);
/* Thick line with round caps (camera guidelines) */
void gfx_line(float x0, float y0, float x1, float y1, float width, col c);
/* Text: n quads (x0,y0,x1,y1,u0,v0,u1,v1) from the font atlas, in a single GPU command */
void gfx_glyphs(const gfx_tex *atlas, const float *quads, int n, col c);

/* Wallpaper (sharp) and its blurred copy for the glass. The blur is computed on the GPU once, when the wallpaper changes. */
int  gfx_set_wallpaper(const gfx_tex *t);
void gfx_blur_params(float k, float sat);   /* before gfx_set_wallpaper: blur strength (1 = blur(30px)) and saturation */
const gfx_tex *gfx_get_wallpaper(void);
void gfx_wallpaper(void);

/* Screen-sized cache layers: everything static is drawn into a layer once, then each frame is a single copy.
 * GFX_L_BASE - bottom layer (wallpaper, column, bottom strip background), GFX_L_SCREEN - cards of the current screen
 * (transparent background; during a transition it is drawn with alpha/shift/scale). */
enum { GFX_L_BASE = 0, GFX_L_SCREEN = 1, GFX_L_SNAP = 2, GFX_LAYERS = 3 };   /* SNAP - screen snapshot for a smooth theme/wallpaper change */
void gfx_layer_begin(int id);   /* further drawing goes into the layer (it is cleared) */
void gfx_layer_end(void);       /* back to the screen */
/* copy of the area (x,y,w,h) of the layer to the screen; s - scale about the area center, dy - shift, alpha - opacity */
void gfx_layer_blit(int id, float x, float y, float w, float h, float alpha, float s, float dy);

/* Common transform for the following primitives (screen transition): scale s about (cx,cy), shift dy,
 * opacity alpha. Computed on the CPU for each primitive; the shaders don't change. */
void gfx_xform(float cx, float cy, float s, float dy, float alpha);
void gfx_xform_reset(void);
/* nested transform on top of the current one (e.g. the call screen inside a screen transition) */
void gfx_xform_push(float cx, float cy, float s, float dy, float alpha);
void gfx_xform_pop(void);

/* Icon/image rotated about the center (degrees) */
void gfx_image_rot(const gfx_tex *t, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1, col tint, float deg);

/* "difference" blend mode (CSS mix-blend-mode: difference) for a white icon over a slider */
void gfx_blend_difference(int on);
void gfx_blend_screen(int on);                 /* mix-blend-mode: screen */
int  gfx_tex_sample(const gfx_tex *t, int w, int h, unsigned char *rgba);   /* downscaled copy into memory */
/* clip to a rectangle (screen px), intersected with the current one; 0 - off */
void gfx_scissor(int on, int x, int y, int w, int h);
void gfx_stats(long *drawn, long *skipped, double *px);   /* how many draw calls were issued / culled (since the last call) */

int  gfx_width(void);
int  gfx_height(void);

#endif
