/* Animations on the REAL clock (pf_now), with the exact CSS cubic-bezier curves from the design. */
#ifndef ANIM_H
#define ANIM_H

typedef struct { float x1, y1, x2, y2; } bezier;
static const bezier EASE        = { 0.25f, 0.1f, 0.25f, 1.0f };   /* CSS ease */
static const bezier EASE_INOUT  = { 0.42f, 0.0f, 0.58f, 1.0f };   /* CSS ease-in-out */
static const bezier EASE_OUT    = { 0.0f, 0.0f, 0.58f, 1.0f };    /* CSS ease-out */
static const bezier LINEAR      = { 0.0f, 0.0f, 1.0f, 1.0f };
static const bezier SPRING      = { 0.34f, 1.56f, 0.64f, 1.0f };  /* design "spring" */
static const bezier SPRING_SOFT = { 0.34f, 1.4f, 0.64f, 1.0f };
static const bezier SPRING_HUD  = { 0.34f, 1.4f, 0.64f, 1.0f };
static const bezier ENTER       = { 0.2f, 0.9f, 0.25f, 1.0f };    /* screen enter */
static const bezier SPRING_NEEDLE = { 0.34f, 1.3f, 0.64f, 1.0f };  /* radio needle, sound sliders */
static const bezier FAN         = { 0.2f, 0.8f, 0.2f, 1.0f };     /* fan rotation */

float bezier_eval(const bezier *b, float t);    /* t 0..1 -> progress (may go outside 0..1) */

/* Value transition like a CSS transition: when the target changes, it glides from the current value. */
typedef struct {
    float from, to, value;
    double t0;
    float ms;
    const bezier *ease;
    float delay_ms;
} tween;

void  tw_init(tween *t, float v);
void  tw_set(tween *t, float target, float ms, const bezier *e, double now);   /* like a CSS property change */
void  tw_set_delay(tween *t, float target, float ms, float delay_ms, const bezier *e, double now);
float tw_get(tween *t, double now);
int   tw_active(const tween *t, double now);

#endif
