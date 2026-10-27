/* Graphics core on OpenGL ES 2: SDF shapes, shadows, glass, images, text, background blur.
 * All GPU resources are created in gfx_init / gfx_set_wallpaper / gfx_tex_load and deleted in gfx_shutdown /
 * gfx_tex_free. Within a frame there are only uniform changes and draw calls, no memory allocation. */
#include "gfx.h"
#include "gl.h"
#include <EGL/egl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG                 /* photos from the USB stick (media.c) */
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO_WRITE
#include "../third_party/stb_image.h"

/* ------------------------------------------------------------------ shaders */

/* highp only where pixel distances are computed (rounding, shadows, glass); everything else is mediump (faster on Vivante) */
#define PREC "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
#define PREC_M "precision mediump float;\n"

/* common vertex shader for rectangular primitives: unit square -> u_rect in screen pixels */
static const char *VS_QUAD =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_rot;\n"   /* cos, sin of the rotation about the center of u_rect (1,0 - no rotation) */
    "varying vec2 v_p;\n"      /* pixels inside u_rect */
    "varying vec2 v_s;\n"      /* screen pixels */
    "varying vec2 v_t;\n"      /* 0..1 inside u_rect */
    "void main() {\n"
    "  vec2 c = u_rect.zw * (a_pos - 0.5);\n"
    "  vec2 s = u_rect.xy + u_rect.zw * 0.5 + vec2(c.x * u_rot.x - c.y * u_rot.y, c.x * u_rot.y + c.y * u_rot.x);\n"
    "  v_p = a_pos * u_rect.zw; v_s = s; v_t = a_pos;\n"
    "  gl_Position = vec4(s.x / u_screen.x * 2.0 - 1.0, 1.0 - s.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";

/* vertex shader for a full-screen pass into an FBO: row orientation is preserved (row 0 -> row 0) */
static const char *VS_FULL =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_t;\n"
    "void main() { v_t = a_pos; gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0); }\n";

/* u_sq = 1: corner is a superellipse |x|^4 + |y|^4 = r^4 (CSS corner-shape: squircle = superellipse(2)), otherwise an arc */
#define SDF_FN \
    "uniform float u_sq;\n" \
    "float sd(vec2 p, vec2 h, float r) {\n" \
    "  vec2 q = abs(p) - (h - vec2(r));\n" \
    "  vec2 m = max(q, 0.0);\n" \
    "  float c = u_sq > 0.5 ? sqrt(sqrt(dot(m * m, m * m))) : length(m);\n" \
    "  return c + min(max(q.x, q.y), 0.0) - r;\n" \
    "}\n"

static const char *FS_SHAPE = PREC SDF_FN
    "varying vec2 v_p;\n"
    "uniform vec2 u_size; uniform float u_r; uniform float u_pad; uniform vec2 u_sub;\n"
    "uniform vec4 u_fill; uniform vec4 u_border; uniform float u_bw;\n"
    "void main() {\n"
    "  vec2 h = u_size * 0.5;\n"
    "  float d = sd(v_p + u_sub - vec2(u_pad) - h, h, u_r);\n"
    "  float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "  vec4 pf = vec4(u_fill.rgb * u_fill.a, u_fill.a);\n"
    "  float band = u_bw > 0.0 ? clamp(d + u_bw + 0.5, 0.0, 1.0) : 0.0;\n"
    "  vec4 pb = vec4(u_border.rgb * u_border.a, u_border.a) * band;\n"
    "  gl_FragColor = (pb + pf * (1.0 - pb.a)) * a;\n"
    "}\n";

static const char *FS_SHADOW = PREC SDF_FN
    "varying vec2 v_p;\n"
    "uniform vec2 u_size; uniform float u_r; uniform float u_pad;\n"
    "uniform vec2 u_off; uniform float u_sigma; uniform vec4 u_color;\n"
    "void main() {\n"
    "  vec2 h = u_size * 0.5;\n"
    "  float d = sd(v_p - vec2(u_pad) - h - u_off, h, u_r);\n"
    "  float a = 1.0 - smoothstep(-2.0 * u_sigma, 2.0 * u_sigma, d);\n"   /* ~ Gaussian falloff, without exp */
    "  gl_FragColor = vec4(u_color.rgb * u_color.a, u_color.a) * a;\n"
    "}\n";

static const char *FS_GLASS = PREC SDF_FN
    "varying vec2 v_p; varying vec2 v_s;\n"
    "uniform vec2 u_size; uniform float u_r; uniform float u_pad;\n"
    "uniform sampler2D u_back; uniform vec2 u_screen;\n"
    "uniform vec4 u_tint; uniform float u_edge; uniform vec4 u_color;\n"
    "void main() {\n"
    "  vec3 c = texture2D(u_back, v_s / u_screen).rgb;\n"          /* background is already blurred and saturated */
    "  c = mix(c, u_tint.rgb, u_tint.a);\n"
    "  vec2 h = u_size * 0.5;\n"
    "  float d = sd(v_p - vec2(u_pad) - h, h, u_r);\n"
    "  float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "  float edge = clamp(1.0 - abs(d + 0.8), 0.0, 1.0);\n"
    "  c = mix(c, vec3(1.0), edge * u_edge);\n"
    "  gl_FragColor = vec4(c, 1.0) * a * u_color.a;\n"
    "}\n";

static const char *FS_IMAGE = PREC SDF_FN
    "varying vec2 v_t; varying vec2 v_p;\n"
    "uniform sampler2D u_tex; uniform vec4 u_uv; uniform vec4 u_tint;\n"
    "uniform vec2 u_size; uniform float u_r;\n"           /* u_r > 0 - rounded corners */
    "void main() {\n"
    "  vec2 uv = mix(u_uv.xy, u_uv.zw, v_t);\n"
    "  float a = 1.0;\n"
    "  if (u_r > 0.0) { vec2 h = u_size * 0.5; a = clamp(0.5 - sd(v_p - h, h, u_r), 0.0, 1.0); }\n"
    "  gl_FragColor = texture2D(u_tex, uv) * vec4(u_tint.rgb * u_tint.a, u_tint.a) * a;\n"
    "}\n";

/* image without rounding (layer compositing, photos): texture coordinates are computed in the vertex shader -
 * the fragment shader does a single direct fetch (on Vivante a "dependent" fetch is noticeably more expensive) */
static const char *VS_BLIT =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect; uniform vec2 u_screen; uniform vec2 u_rot; uniform vec4 u_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 c = u_rect.zw * (a_pos - 0.5);\n"
    "  vec2 s = u_rect.xy + u_rect.zw * 0.5 + vec2(c.x * u_rot.x - c.y * u_rot.y, c.x * u_rot.y + c.y * u_rot.x);\n"
    "  v_uv = mix(u_uv.xy, u_uv.zw, a_pos);\n"
    "  gl_Position = vec4(s.x / u_screen.x * 2.0 - 1.0, 1.0 - s.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *FS_BLIT = PREC
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex; uniform vec4 u_tint;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv) * vec4(u_tint.rgb * u_tint.a, u_tint.a); }\n";

/* solid fill with no shape (middles of rounded rectangles, dimming): the cheapest pixel */
static const char *FS_FILL = PREC
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

static const char *VS_GLYPH =
    "attribute vec2 a_xy; attribute vec2 a_uv;\n"
    "uniform vec2 u_screen;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv;\n"
    "  gl_Position = vec4(a_xy.x / u_screen.x * 2.0 - 1.0, 1.0 - a_xy.y / u_screen.y * 2.0, 0.0, 1.0); }\n";

static const char *FS_GLYPH = PREC_M
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex; uniform vec4 u_color;\n"
    /* glyph coverage with gamma correction ~ like a browser (light text on dark doesn't "thin out") */
    "void main() { float a = pow(texture2D(u_tex, v_uv).a, 0.8);\n"
    "  gl_FragColor = vec4(u_color.rgb * u_color.a, u_color.a) * a; }\n";

/* copy with 2x downscale: one bilinear fetch at the center between 4 texels = 2x2 average */
static const char *FS_COPY = PREC_M
    "varying vec2 v_t;\n"
    "uniform sampler2D u_src;\n"
    "void main() { gl_FragColor = texture2D(u_src, v_t); }\n";

/* separable Gaussian, sigma = 6 texels: 13 fetches between neighboring texels (the GPU averages pairs).
 * u_sat - saturation (the design's saturate is applied once, in the last pass) */
static const char *FS_BLUR = PREC_M
    "varying vec2 v_t;\n"
    "uniform sampler2D u_src; uniform vec2 u_step; uniform float u_sat;\n"
    "void main() {\n"
    "  vec4 s = texture2D(u_src, v_t) * 0.06904;\n"
    "  s += (texture2D(u_src, v_t + u_step * 1.4896)  + texture2D(u_src, v_t - u_step * 1.4896))  * 0.13341;\n"
    "  s += (texture2D(u_src, v_t + u_step * 3.4757)  + texture2D(u_src, v_t - u_step * 3.4757))  * 0.11622;\n"
    "  s += (texture2D(u_src, v_t + u_step * 5.4619)  + texture2D(u_src, v_t - u_step * 5.4619))  * 0.09066;\n"
    "  s += (texture2D(u_src, v_t + u_step * 7.4482)  + texture2D(u_src, v_t - u_step * 7.4482))  * 0.06334;\n"
    "  s += (texture2D(u_src, v_t + u_step * 9.4344)  + texture2D(u_src, v_t - u_step * 9.4344))  * 0.03964;\n"
    "  s += (texture2D(u_src, v_t + u_step * 11.4206) + texture2D(u_src, v_t - u_step * 11.4206)) * 0.02221;\n"
    "  float l = dot(s.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "  gl_FragColor = vec4(clamp(mix(vec3(l), s.rgb, u_sat), 0.0, 1.0), 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ programs */

typedef struct {
    GLuint prog;
    GLint u_rect, u_screen, u_rot, u_size, u_r, u_pad;
    GLint u_fill, u_border, u_bw;            /* shape */
    GLint u_off, u_sigma, u_color;           /* shadow, glyph */
    GLint u_back, u_tint, u_edge;            /* glass */
    GLint u_tex, u_uv;                       /* image, glyph */
    GLint u_src, u_step, u_sat;              /* copy, blur */
    GLint u_sub;                             /* shape: offset of the piece within the full rectangle */
    GLint u_sq;                              /* squircle corners (SDF_FN) */
} program;

enum { P_SHAPE, P_SHADOW, P_GLASS, P_IMAGE, P_GLYPH, P_COPY, P_BLUR, P_BLIT, P_FILL, P_COUNT };
static program P[P_COUNT];
static GLuint quad_vbo;
static int scr_w, scr_h;
static int cur = -1;

/* blurred background: 512x300 -> 256x150 (a) -> Gaussian X (b) -> Gaussian Y + saturate (a) */
static GLuint fbo[3], ftex[3];
static const int FW[3] = { 512, 256, 256 }, FH[3] = { 300, 150, 150 };
static const gfx_tex *wall;
static float blur_k = 1, blur_sat = 1.6f;          /* gfx_blur_params */

/* screen-sized cache layers */
static GLuint layer_fbo[GFX_LAYERS], layer_tex[GFX_LAYERS];

/* transform (CPU): x' = xf_s * x + xf_bx, y' = xf_s * y + xf_by; opacity xf_a. Nested (stack). */
static float xf_s = 1, xf_bx, xf_by, xf_a = 1;
#define XF_DEPTH 4
static float xf_stack[XF_DEPTH][4];
static int xf_n;
static float rot_c = 1, rot_s = 0;                 /* rotation of the current primitive (cos, sin) */
static float img_r;                                 /* image rounding */
static float sq_on;                                 /* gfx_squircle: superellipse corners */
void gfx_squircle(int on) { sq_on = on ? 1.0f : 0.0f; }
static void xf(float *x, float *y, float *w, float *h)
{
    if (xf_s == 1 && xf_bx == 0 && xf_by == 0) return;
    *x = xf_s * *x + xf_bx; *y = xf_s * *y + xf_by;
    *w *= xf_s; *h *= xf_s;
}

void gfx_xform(float cx, float cy, float s, float dy, float alpha)
{
    xf_n = 0;
    xf_s = s; xf_bx = cx - cx * s; xf_by = cy - cy * s + dy; xf_a = alpha;
}
void gfx_xform_push(float cx, float cy, float s, float dy, float alpha)
{
    if (xf_n < XF_DEPTH) { xf_stack[xf_n][0] = xf_s; xf_stack[xf_n][1] = xf_bx; xf_stack[xf_n][2] = xf_by; xf_stack[xf_n][3] = xf_a; xf_n++; }
    xf_bx = xf_s * (cx - cx * s) + xf_bx;
    xf_by = xf_s * (cy - cy * s + dy) + xf_by;
    xf_s *= s; xf_a *= alpha;
}
void gfx_xform_pop(void)
{
    if (xf_n > 0) { xf_n--; xf_s = xf_stack[xf_n][0]; xf_bx = xf_stack[xf_n][1]; xf_by = xf_stack[xf_n][2]; xf_a = xf_stack[xf_n][3]; }
}
void gfx_xform_reset(void) { xf_n = 0; xf_s = 1; xf_bx = 0; xf_by = 0; xf_a = 1; }

static GLuint compile(GLenum type, const char *src)
{
    GLint ok = 0;
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "hu: shader compile error:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int link(program *p, const char *vs, const char *fs, int glyph)
{
    GLint ok = 0;
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    memset(p, 0, sizeof *p);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return -1; }
    p->prog = glCreateProgram();
    glAttachShader(p->prog, v);
    glAttachShader(p->prog, f);
    if (glyph) { glBindAttribLocation(p->prog, 0, "a_xy"); glBindAttribLocation(p->prog, 1, "a_uv"); }
    else glBindAttribLocation(p->prog, 0, "a_pos");
    glLinkProgram(p->prog);
    glDeleteShader(v);                       /* shaders are no longer needed - they stay in the program */
    glDeleteShader(f);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "hu: program link error:\n%s\n", log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        return -1;
    }
#define U(n) p->n = glGetUniformLocation(p->prog, #n)
    U(u_sq); U(u_rect); U(u_screen); U(u_rot); U(u_size); U(u_r); U(u_pad); U(u_fill); U(u_border); U(u_bw);
    U(u_off); U(u_sigma); U(u_color); U(u_back); U(u_tint); U(u_edge);
    U(u_tex); U(u_uv); U(u_src); U(u_step); U(u_sat); U(u_sub);
#undef U
    return 0;
}

static program *use(int i)
{
    program *p = &P[i];
    if (cur != i) {
        glUseProgram(p->prog);
        if (p->u_screen >= 0) glUniform2f(p->u_screen, (float)scr_w, (float)scr_h);
        cur = i;
    }
    return p;
}

static void quad_attr(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glDisableVertexAttribArray(1);
}

static void draw_quad(program *p, float x, float y, float w, float h)
{
    glUniform4f(p->u_rect, x, y, w, h);
    if (p->u_rot >= 0) glUniform2f(p->u_rot, rot_c, rot_s);
    quad_attr();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ------------------------------------------------------------------ init / shutdown */

int gfx_init(int screen_w, int screen_h)
{
    static const float q[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    int i;
    scr_w = screen_w; scr_h = screen_h;
    fprintf(stderr, "hu: GL %s | %s\nhu: GL ext: %s\n", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_EXTENSIONS));
    if (link(&P[P_SHAPE], VS_QUAD, FS_SHAPE, 0) || link(&P[P_SHADOW], VS_QUAD, FS_SHADOW, 0) ||
        link(&P[P_GLASS], VS_QUAD, FS_GLASS, 0) || link(&P[P_IMAGE], VS_QUAD, FS_IMAGE, 0) ||
        link(&P[P_BLIT], VS_BLIT, FS_BLIT, 0) || link(&P[P_FILL], VS_QUAD, FS_FILL, 0) ||
        link(&P[P_GLYPH], VS_GLYPH, FS_GLYPH, 1) || link(&P[P_COPY], VS_FULL, FS_COPY, 0) ||
        link(&P[P_BLUR], VS_FULL, FS_BLUR, 0)) {
        gfx_shutdown();
        return -1;
    }
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);

    glGenFramebuffers(3, fbo);
    glGenTextures(3, ftex);
    for (i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, ftex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FW[i], FH[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ftex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "hu: FBO %d incomplete\n", i);
            gfx_shutdown();
            return -1;
        }
    }
    glGenFramebuffers(GFX_LAYERS, layer_fbo);
    glGenTextures(GFX_LAYERS, layer_tex);
    for (i = 0; i < GFX_LAYERS; i++) {
        glBindTexture(GL_TEXTURE_2D, layer_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, scr_w, scr_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, layer_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer_tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "hu: layer FBO %d incomplete\n", i);
            gfx_shutdown();
            return -1;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

static gfx_tex ramp;          /* ramp for gfx_vgrad */

void gfx_shutdown(void)
{
    if (ramp.id) gfx_tex_free(&ramp);
    int i;
    for (i = 0; i < P_COUNT; i++) if (P[i].prog) { glDeleteProgram(P[i].prog); P[i].prog = 0; }
    if (quad_vbo) { glDeleteBuffers(1, &quad_vbo); quad_vbo = 0; }
    if (fbo[0]) { glDeleteFramebuffers(3, fbo); memset(fbo, 0, sizeof fbo); }
    if (ftex[0]) { glDeleteTextures(3, ftex); memset(ftex, 0, sizeof ftex); }
    if (layer_fbo[0]) { glDeleteFramebuffers(GFX_LAYERS, layer_fbo); memset(layer_fbo, 0, sizeof layer_fbo); }
    if (layer_tex[0]) { glDeleteTextures(GFX_LAYERS, layer_tex); memset(layer_tex, 0, sizeof layer_tex); }
    wall = NULL;
    cur = -1;
}

int gfx_width(void) { return scr_w; }
int gfx_height(void) { return scr_h; }

void gfx_finish(void) { glFinish(); }

/* copy of a texture (video frame) into our own screen-sized RGBA: the VPU frame must be returned to the decoder, but there is still something to show */
int gfx_tex_snapshot(gfx_tex *dst, const gfx_tex *src, float u1, float v1)
{
    static GLuint fbo;
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    col c = { 1, 1, 1, 1 };
    if (!dst->id) {
        glGenTextures(1, &dst->id);
        glBindTexture(GL_TEXTURE_2D, dst->id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, scr_w, scr_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        dst->w = scr_w; dst->h = scr_h;
        if (fbo) glDeleteFramebuffers(1, &fbo);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->id, 0);
    } else glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, scr_w, scr_h);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    gfx_xform_reset();
    gfx_image(src, 0, 0, (float)scr_w, (float)scr_h, 0, v1, u1, 0, c);   /* v is reversed: in the FBO the top is at t=1 -> the copy reads like a normal one */
    glEnable(GL_BLEND);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cur = -1;
    return 0;
}

void gfx_frame_begin(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, scr_w, scr_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);     /* premultiplied alpha */
    cur = -1;
}

void gfx_frame_end(void)
{
#ifdef HU_GL_DEBUG
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "hu: GL error 0x%x\n", e);
#endif
}

/* ------------------------------------------------------------------ CPU culling
 * The frame is drawn per changed area (scissor); anything entirely outside the current area is not handed to the driver:
 * on Vivante every draw call is expensive for the CPU. Coordinates are already after xf() (screen px, y down). */
static int sc_on, sc_x0, sc_y0, sc_x1, sc_y1;
static long n_draw, n_cull;
static double n_px;                                /* painted area (px), for estimating the GPU load */

static int culled(float x, float y, float w, float h)
{
    if (rot_s != 0) {                              /* rotated: the bounding square */
        float cx = x + w * 0.5f, cy = y + h * 0.5f, r = sqrtf(w * w + h * h) * 0.5f;
        x = cx - r; y = cy - r; w = h = r * 2;
    }
    if (sc_on && (x + w < sc_x0 || y + h < sc_y0 || x > sc_x1 || y > sc_y1)) { n_cull++; return 1; }
    n_draw++;
    {
        float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        if (sc_on) { x0 = fmaxf(x0, sc_x0); y0 = fmaxf(y0, sc_y0); x1 = fminf(x1, sc_x1); y1 = fminf(y1, sc_y1); }
        if (x1 > x0 && y1 > y0) n_px += (double)(x1 - x0) * (y1 - y0);
    }
    return 0;
}

void gfx_stats(long *drawn, long *skipped, double *px) { *drawn = n_draw; *skipped = n_cull; *px = n_px; n_draw = n_cull = 0; n_px = 0; }

/* ------------------------------------------------------------------ primitives */

static void fill_quad(float x, float y, float w, float h, col c)       /* premultiplied */
{
    program *p = use(P_FILL);
    glUniform4f(p->u_color, c.r * c.a, c.g * c.a, c.b * c.a, c.a);
    draw_quad(p, x, y, w, h);
}

static void shape_part(program *p, float ox, float oy, float x, float y, float w, float h)
{
    glUniform2f(p->u_sub, x - ox, y - oy);
    draw_quad(p, x, y, w, h);
}

void gfx_rect_border(float x, float y, float w, float h, float radius, col fill, float bw, col border)
{
    program *p;
    float r, m;
    radius *= xf_s; bw *= xf_s; fill.a *= xf_a; border.a *= xf_a;
    xf(&x, &y, &w, &h);
    if (xf_s == 1 && rot_s == 0) {                 /* static: edges on whole pixels (crisp 1px lines/borders,
                                                      no scale/rotation - animations stay smooth) */
        float x1 = roundf(x + w), y1 = roundf(y + h);
        x = roundf(x); y = roundf(y);
        w = x1 - x; if (w < 1) w = 1;
        h = y1 - y; if (h < 1) h = 1;
    }
    if (culled(x - 1, y - 1, w + 2, h + 2)) return;
    r = radius < 0 ? 0 : (radius > w * 0.5f ? w * 0.5f : radius);
    if (r > h * 0.5f) r = h * 0.5f;
    /* no rounding or border, on whole pixels - plain fill */
    if (r < 0.5f && bw <= 0 && rot_s == 0 && x == floorf(x) && y == floorf(y) && w == floorf(w) && h == floorf(h)) {
        if (fill.a > 0) fill_quad(x, y, w, h, fill);
        return;
    }
    p = use(P_SHAPE);
    glUniform2f(p->u_size, w, h);
    glUniform1f(p->u_r, r);
    glUniform1f(p->u_sq, sq_on);
    glUniform1f(p->u_pad, 1.0f);
    glUniform4f(p->u_fill, fill.r, fill.g, fill.b, fill.a);
    glUniform4f(p->u_border, border.r, border.g, border.b, border.a);
    glUniform1f(p->u_bw, bw);
    /* large rectangle: SDF only along the edge (width m), the middle is a cheap fill.
     * The pieces share edges with identical coordinates - no gaps and no double blending. */
    m = ceilf(fmaxf(r, bw)) + 1;
    if (rot_s == 0 && w > 2 * m + 8 && h > 2 * m + 8 && w * h > 6000 && fill.a > 0) {
        float ox = x - 1, oy = y - 1;
        shape_part(p, ox, oy, x - 1, y - 1, w + 2, m + 1);                 /* top */
        shape_part(p, ox, oy, x - 1, y + h - m, w + 2, m + 1);             /* bottom */
        shape_part(p, ox, oy, x - 1, y + m, m + 1, h - 2 * m);             /* left */
        shape_part(p, ox, oy, x + w - m, y + m, m + 1, h - 2 * m);         /* right */
        fill_quad(x + m, y + m, w - 2 * m, h - 2 * m, fill);
        return;
    }
    glUniform2f(p->u_sub, 0, 0);
    draw_quad(p, x - 1, y - 1, w + 2, h + 2);         /* +1px for edge smoothing */
}

void gfx_rect(float x, float y, float w, float h, float radius, col fill)
{
    col none = { 0, 0, 0, 0 };
    gfx_rect_border(x, y, w, h, radius, fill, 0, none);
}

void gfx_shadow(float x, float y, float w, float h, float radius, float blur, float ox, float oy, col c)
{
    program *p;
    float m;
    radius *= xf_s; blur *= xf_s; ox *= xf_s; oy *= xf_s; c.a *= xf_a;
    xf(&x, &y, &w, &h);
    m = blur * 1.5f + fmaxf(fabsf(ox), fabsf(oy)) + 1;
    if (culled(x - m, y - m, w + 2 * m, h + 2 * m)) return;
    p = use(P_SHADOW);
    float r = fminf(radius, fminf(w, h) * 0.5f);
    glUniform2f(p->u_size, w, h);
    glUniform1f(p->u_r, r);
    glUniform1f(p->u_sq, sq_on);
    glUniform1f(p->u_pad, m);
    glUniform2f(p->u_off, ox, oy);
    glUniform1f(p->u_sigma, fmaxf(0.5f, blur * 0.5f));
    glUniform4f(p->u_color, c.r, c.g, c.b, c.a);
    draw_quad(p, x - m, y - m, w + 2 * m, h + 2 * m);
}

void gfx_glass(float x, float y, float w, float h, float radius, col tint, float tint_a, float edge_a)
{
    program *p;
    float r;
    radius *= xf_s;
    xf(&x, &y, &w, &h);
    if (culled(x - 1, y - 1, w + 2, h + 2)) return;
    r = fminf(radius, fminf(w, h) * 0.5f);
    if (!wall) { gfx_rect(x, y, w, h, r, col_alpha(tint, tint_a)); return; }
    p = use(P_GLASS);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ftex[1]);
    glUniform1i(p->u_back, 0);
    glUniform2f(p->u_size, w, h);
    glUniform1f(p->u_r, r);
    glUniform1f(p->u_sq, sq_on);
    glUniform1f(p->u_pad, 1.0f);
    glUniform4f(p->u_tint, tint.r, tint.g, tint.b, tint_a);
    glUniform1f(p->u_edge, edge_a);
    if (p->u_color >= 0) glUniform4f(p->u_color, 1, 1, 1, xf_a);
    draw_quad(p, x - 1, y - 1, w + 2, h + 2);
}

void gfx_image(const gfx_tex *t, float x, float y, float w, float h,
               float u0, float v0, float u1, float v1, col tint)
{
    program *p;
    tint.a *= xf_a;
    xf(&x, &y, &w, &h);
    if (culled(x, y, w, h)) return;
    p = use(img_r > 0 ? P_IMAGE : P_BLIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glUniform1i(p->u_tex, 0);
    glUniform4f(p->u_uv, u0, v0, u1, v1);
    glUniform4f(p->u_tint, tint.r, tint.g, tint.b, tint.a);
    if (img_r > 0) { glUniform1f(p->u_r, img_r * xf_s); glUniform2f(p->u_size, w, h); glUniform1f(p->u_sq, sq_on); }
    draw_quad(p, x, y, w, h);
}

void gfx_image_round(const gfx_tex *t, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, col tint, float radius)
{
    img_r = radius;
    gfx_image(t, x, y, w, h, u0, v0, u1, v1, tint);
    img_r = 0;
}

void gfx_line(float x0, float y0, float x1, float y1, float width, col c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    rot_c = dx / len; rot_s = dy / len;                 /* rotation along the segment: cos/sin without trigonometry */
    gfx_rect((x0 + x1) * 0.5f - (len + width) * 0.5f, (y0 + y1) * 0.5f - width * 0.5f, len + width, width, width * 0.5f, c);
    rot_c = 1; rot_s = 0;
}

void gfx_image_rot(const gfx_tex *t, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1, col tint, float deg)
{
    float a = deg * 3.14159265f / 180.0f;
    rot_c = cosf(a); rot_s = sinf(a);
    gfx_image(t, x, y, w, h, u0, v0, u1, v1, tint);
    rot_c = 1; rot_s = 0;
}

#define MAX_GLYPHS 256
void gfx_glyphs(const gfx_tex *atlas, const float *quads, int n, col c)
{
    static float v[MAX_GLYPHS * 6 * 4];            /* 6 vertices * (x, y, u, v) */
    program *p;
    int i, k = 0;
    if (n > MAX_GLYPHS) n = MAX_GLYPHS;
    if (n <= 0) return;
    {   /* line bounds - for culling */
        float x0 = quads[0], y0 = quads[1], x1 = quads[2], y1 = quads[3], w, h;
        for (i = 1; i < n; i++) {
            const float *q = quads + i * 8;
            if (q[0] < x0) x0 = q[0];
            if (q[1] < y0) y0 = q[1];
            if (q[2] > x1) x1 = q[2];
            if (q[3] > y1) y1 = q[3];
        }
        w = x1 - x0; h = y1 - y0;
        xf(&x0, &y0, &w, &h);
        if (culled(x0, y0, w, h)) return;
    }
    p = use(P_GLYPH);
    for (i = 0; i < n; i++) {
        const float *q = quads + i * 8;            /* x0 y0 x1 y1 u0 v0 u1 v1 */
        float g[8];
        memcpy(g, q, sizeof g);
        if (xf_s != 1 || xf_bx != 0 || xf_by != 0) {
            g[0] = xf_s * g[0] + xf_bx; g[2] = xf_s * g[2] + xf_bx;
            g[1] = xf_s * g[1] + xf_by; g[3] = xf_s * g[3] + xf_by;
        }
        {
        const float tri[6][4] = {
            { g[0], g[1], g[4], g[5] }, { g[2], g[1], g[6], g[5] }, { g[0], g[3], g[4], g[7] },
            { g[2], g[1], g[6], g[5] }, { g[2], g[3], g[6], g[7] }, { g[0], g[3], g[4], g[7] } };
        memcpy(v + k, tri, sizeof tri);
        k += 24;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas->id);
    glUniform1i(p->u_tex, 0);
    glUniform4f(p->u_color, c.r, c.g, c.b, c.a * xf_a);
    glBindBuffer(GL_ARRAY_BUFFER, 0);              /* client arrays */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
    glDisableVertexAttribArray(1);
}

/* ------------------------------------------------------------------ textures */

static col load_ov;

int gfx_tex_load(gfx_tex *t, const char *path, int mipmaps)
{
    int w, h, n, i, pot, ch;
    unsigned char *px;
    GLenum fmt;
    memset(t, 0, sizeof *t);
    if (!stbi_info(path, &w, &h, &n)) { fprintf(stderr, "hu: cannot load %s\n", path); return -1; }
    ch = n == 1 ? 1 : 4;                           /* 1 channel (font atlases) -> GL_ALPHA, otherwise RGBA */
    px = stbi_load(path, &w, &h, &n, ch);
    if (!px) { fprintf(stderr, "hu: cannot load %s\n", path); return -1; }
    if (ch == 4 && load_ov.a > 0)                  /* color overlay on top of the image */
        for (i = 0; i < w * h * 4; i += 4) {
            px[i + 0] = (unsigned char)(px[i + 0] * (1 - load_ov.a) + load_ov.r * 255 * load_ov.a + 0.5f);
            px[i + 1] = (unsigned char)(px[i + 1] * (1 - load_ov.a) + load_ov.g * 255 * load_ov.a + 0.5f);
            px[i + 2] = (unsigned char)(px[i + 2] * (1 - load_ov.a) + load_ov.b * 255 * load_ov.a + 0.5f);
        }
    if (ch == 4)
        for (i = 0; i < w * h; i++) {              /* premultiplied alpha */
            unsigned a = px[i * 4 + 3];
            px[i * 4 + 0] = (unsigned char)((px[i * 4 + 0] * a + 127) / 255);
            px[i * 4 + 1] = (unsigned char)((px[i * 4 + 1] * a + 127) / 255);
            px[i * 4 + 2] = (unsigned char)((px[i * 4 + 2] * a + 127) / 255);
        }
    fmt = ch == 1 ? GL_ALPHA : GL_RGBA;
    pot = !(w & (w - 1)) && !(h & (h - 1));
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mipmaps && pot) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    t->w = w; t->h = h;
    return 0;
}

/* RGBA from memory (photos from the USB stick, already downscaled): premultiplied alpha is not needed - JPEG is opaque */
int gfx_tex_rgba(gfx_tex *t, int w, int h, const unsigned char *px)
{
    memset(t, 0, sizeof *t);
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    t->w = w; t->h = h;
    return 0;
}

/* NV12 video frame without copying (Vivante GL_VIV_tex_direct): the GPU reads the VPU buffer (by physical address or
 * maps the virtual one itself), and does YUV -> RGB itself. w - Y row stride, h - Y plane height (with alignment).
 * The extension is absent on the PC: -1. */
#define GL_VIV_NV12 0x8FC1
#define GL_VIV_YUY2 0x8FC2
static int tex_direct(gfx_tex *t, int w, int h, const void *vaddr, unsigned paddr, GLenum fmt);
int gfx_tex_yuv(gfx_tex *t, int w, int h, const void *vaddr, unsigned paddr) { return tex_direct(t, w, h, vaddr, paddr, GL_VIV_NV12); }
int gfx_tex_yuyv(gfx_tex *t, int w, int h, const void *vaddr, unsigned paddr) { return tex_direct(t, w, h, vaddr, paddr, GL_VIV_YUY2); }

static int tex_direct(gfx_tex *t, int w, int h, const void *vaddr, unsigned paddr, GLenum fmt)
{
    static void (*map)(GLenum, GLsizei, GLsizei, GLenum, GLvoid **, const GLuint *);
    static void (*inval)(GLenum);
    static int tried;
    void *v = (void *)vaddr;
    if (!tried) {
        tried = 1;
        map = (void (*)(GLenum, GLsizei, GLsizei, GLenum, GLvoid **, const GLuint *))eglGetProcAddress("glTexDirectVIVMap");
        inval = (void (*)(GLenum))eglGetProcAddress("glTexDirectInvalidateVIV");
        if (!map || !inval) fprintf(stderr, "hu: gfx: no GL_VIV_tex_direct\n");
    }
    if (!map || !inval) return -1;
    if (!t->id) {
        glGenTextures(1, &t->id);
        glBindTexture(GL_TEXTURE_2D, t->id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else glBindTexture(GL_TEXTURE_2D, t->id);
    while (glGetError() != GL_NO_ERROR) {}
    map(GL_TEXTURE_2D, w, h, fmt, &v, &paddr);
    inval(GL_TEXTURE_2D);
    t->w = w; t->h = h;
    {
        GLenum e = glGetError();
        static int nerr;
        if (e != GL_NO_ERROR && nerr++ < 3) fprintf(stderr, "hu: gfx: glTexDirectVIVMap %dx%d fmt 0x%x -> 0x%x\n", w, h, fmt, e);
        return e == GL_NO_ERROR ? 0 : -1;
    }
}

/* the buffer of the already bound YUV texture was overwritten (new VPU frame) - the GPU must re-read it */
void gfx_tex_yuv_dirty(gfx_tex *t)
{
    static void (*inval)(GLenum);
    if (!inval) inval = (void (*)(GLenum))eglGetProcAddress("glTexDirectInvalidateVIV");
    if (!inval || !t->id) return;
    glBindTexture(GL_TEXTURE_2D, t->id);
    inval(GL_TEXTURE_2D);
}

/* empty single-channel texture (glyph cache), filled in pieces */
int gfx_tex_alpha(gfx_tex *t, int w, int h)
{
    unsigned char *z = calloc((size_t)w * (size_t)h, 1);
    if (!z) return -1;
    memset(t, 0, sizeof *t);
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, z);
    free(z);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    t->w = w; t->h = h;
    return 0;
}

void gfx_tex_alpha_sub(gfx_tex *t, int x, int y, int w, int h, const unsigned char *px)
{
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_ALPHA, GL_UNSIGNED_BYTE, px);
}

/* vertical gradient of color c: from full opacity c.a (at the top if top_opaque) to 0.
 * The ramp is in a GL_ALPHA texture, drawn by the glyph shader (accounting for its pow 0.8). */
static int ramp_ready(void)
{
    if (!ramp.id) {
        unsigned char px[256 * 2];
        int i;
        for (i = 0; i < 256; i++) px[i * 2] = px[i * 2 + 1] = (unsigned char)(powf(1 - i / 255.0f, 1.25f) * 255 + 0.5f);
        if (gfx_tex_alpha(&ramp, 2, 256)) return 0;
        gfx_tex_alpha_sub(&ramp, 0, 0, 2, 256, px);
    }
    return 1;
}

/* the same with rounded corners (header dimming over a photo, with the card radius) */
void gfx_vgrad_round(float x, float y, float w, float h, col c, int top_opaque, float radius)
{
    if (!ramp_ready()) return;
    gfx_image_round(&ramp, x, y, w, h, 0.5f, top_opaque ? 0.5f / 256 : 255.5f / 256, 0.5f, top_opaque ? 255.5f / 256 : 0.5f / 256, c, radius);
}

void gfx_vgrad(float x, float y, float w, float h, col c, int top_opaque)
{
    float q[8];
    if (!ramp_ready()) return;
    q[0] = x; q[1] = y; q[2] = x + w; q[3] = y + h;
    q[4] = 0.5f; q[6] = 0.5f;
    q[5] = top_opaque ? 0.5f / 256 : 255.5f / 256; q[7] = top_opaque ? 255.5f / 256 : 0.5f / 256;
    gfx_glyphs(&ramp, q, 1, c);
}

void gfx_tex_free(gfx_tex *t)
{
    if (t->id) glDeleteTextures(1, &t->id);
    if (wall == t) wall = NULL;
    memset(t, 0, sizeof *t);
}

/* ------------------------------------------------------------------ wallpaper and blur (on the GPU, once) */

static void pass(int dst, GLuint src_tex, int prog, float sx, float sy, float sat)
{
    program *p = use(prog);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[dst]);
    glViewport(0, 0, FW[dst], FH[dst]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(p->u_src, 0);
    if (prog == P_BLUR) { glUniform2f(p->u_step, sx, sy); glUniform1f(p->u_sat, sat); }
    quad_attr();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

const gfx_tex *gfx_get_wallpaper(void) { return wall; }
void gfx_blur_params(float k, float sat) { blur_k = k; blur_sat = sat; }

int gfx_set_wallpaper(const gfx_tex *t)
{
    wall = t;
    if (!t) return 0;
    glDisable(GL_BLEND);
    cur = -1;
    pass(0, t->id, P_COPY, 0, 0, 1);                           /* 1024x600 -> 512x300 */
    pass(1, ftex[0], P_COPY, 0, 0, 1);                         /* -> 256x150 */
    pass(2, ftex[1], P_BLUR, blur_k / FW[1], 0, 1.0f);         /* Gaussian X (sigma 6 texels x blur_k) */
    pass(1, ftex[2], P_BLUR, 0, blur_k / FH[1], blur_sat);     /* Gaussian Y + saturate */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, scr_w, scr_h);
    glEnable(GL_BLEND);
    cur = -1;
    return 0;
}

void gfx_wallpaper(void)
{
    col white = { 1, 1, 1, 1 };
    if (!wall) return;
    glDisable(GL_BLEND);
    gfx_image(wall, 0, 0, (float)scr_w, (float)scr_h, 0, 0, 1, 1, white);
    glEnable(GL_BLEND);
}

/* ------------------------------------------------------------------ static layer */

void gfx_layer_begin(int id)
{
    glBindFramebuffer(GL_FRAMEBUFFER, layer_fbo[id]);
    glViewport(0, 0, scr_w, scr_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0, 0, 0, 0);                      /* transparent background: the screen layer is placed over the bottom one */
    glClear(GL_COLOR_BUFFER_BIT);
    gfx_xform_reset();
    cur = -1;
}

void gfx_layer_end(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, scr_w, scr_h);
    cur = -1;
}

void gfx_layer_blit(int id, float x, float y, float w, float h, float alpha, float s, float dy)
{
    /* in the FBO the top of the screen ended up at row t=1 (GL convention) - sample with a flip in v */
    gfx_tex t = { layer_tex[id], scr_w, scr_h };
    col c = { 1, 1, 1, alpha };
    float u0 = x / scr_w, u1 = (x + w) / scr_w, v0 = 1 - y / scr_h, v1 = 1 - (y + h) / scr_h;
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    int opaque = id == GFX_L_BASE || (id == GFX_L_SNAP && alpha >= 0.999f);
    if (alpha <= 0.002f) return;
    if (opaque) glDisable(GL_BLEND);
    gfx_xform_reset();
    gfx_image(&t, cx + (x - cx) * s, cy + (y - cy) * s + dy, w * s, h * s, u0, v0, u1, v1, c);
    if (opaque) glEnable(GL_BLEND);
}

/* mix-blend-mode: screen for premultiplied src: src + dst*(1-src) */
void gfx_blend_screen(int on)
{
    if (on) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
    else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

/* downscaled copy of a w x h texture into memory (RGBA) - e.g. wallpaper colors */
int gfx_tex_sample(const gfx_tex *t, int w, int h, unsigned char *rgba)
{
    GLuint fbo, tex;
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    col c = { 1, 1, 1, 1 };
    if (!t || !t->id) return -1;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, w, h);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    gfx_xform_reset();
    gfx_image(t, 0, 0, (float)scr_w, (float)scr_h, 0, 0, 1, 1, c);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glEnable(GL_BLEND);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, scr_w, scr_h);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    cur = -1;
    return 0;
}

void gfx_blend_difference(int on)
{
    /* src*(1-dst) + dst*(1-src): for white src with coverage a gives |dst - a| (like mix-blend-mode: difference) */
    if (on) glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);
    else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void gfx_scissor(int on, int x, int y, int w, int h)
{
    sc_on = on; sc_x0 = x; sc_y0 = y; sc_x1 = x + w; sc_y1 = y + h;
    if (!on) { glDisable(GL_SCISSOR_TEST); return; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, scr_h - (y + h), w < 0 ? 0 : w, h < 0 ? 0 : h);
}

int gfx_tex_load_overlay(gfx_tex *t, const char *path, col overlay)
{
    int r;
    load_ov = overlay;
    r = gfx_tex_load(t, path, 0);
    load_ov.a = 0;
    return r;
}
