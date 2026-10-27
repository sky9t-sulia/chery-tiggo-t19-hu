#include "anim.h"

#include <math.h>

/* CSS cubic-bezier: x(s) = 3(1-s)^2 s x1 + 3(1-s) s^2 x2 + s^3; solve for s given x by Newton's method + bisection */
static float bx(const bezier *b, float s) { float u = 1 - s; return 3 * u * u * s * b->x1 + 3 * u * s * s * b->x2 + s * s * s; }
static float by(const bezier *b, float s) { float u = 1 - s; return 3 * u * u * s * b->y1 + 3 * u * s * s * b->y2 + s * s * s; }
static float bdx(const bezier *b, float s) { float u = 1 - s; return 3 * u * u * b->x1 + 6 * u * s * (b->x2 - b->x1) + 3 * s * s * (1 - b->x2); }

float bezier_eval(const bezier *b, float t)
{
    float s = t, lo = 0, hi = 1;
    int i;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    for (i = 0; i < 8; i++) {
        float x = bx(b, s) - t, d = bdx(b, s);
        if (fabsf(x) < 1e-5f) return by(b, s);
        if (fabsf(d) < 1e-6f) break;
        s -= x / d;
    }
    s = t;
    for (i = 0; i < 30; i++) {
        float x = bx(b, s);
        if (fabsf(x - t) < 1e-5f) break;
        if (x < t) lo = s; else hi = s;
        s = (lo + hi) * 0.5f;
    }
    return by(b, s);
}

void tw_init(tween *t, float v)
{
    t->from = t->to = t->value = v; t->t0 = 0; t->ms = 0; t->ease = &LINEAR; t->delay_ms = 0;
}

void tw_set_delay(tween *t, float target, float ms, float delay_ms, const bezier *e, double now)
{
    if (target == t->to) return;
    t->from = tw_get(t, now);
    t->to = target; t->t0 = now; t->ms = ms; t->ease = e; t->delay_ms = delay_ms;
}

void tw_set(tween *t, float target, float ms, const bezier *e, double now)
{
    tw_set_delay(t, target, ms, 0, e, now);
}

float tw_get(tween *t, double now)
{
    float k;
    if (t->ms <= 0) { t->value = t->to; return t->value; }
    k = (float)((now - t->t0) * 1000.0 - t->delay_ms) / t->ms;
    if (k <= 0) t->value = t->from;
    else if (k >= 1) t->value = t->to;
    else t->value = t->from + (t->to - t->from) * bezier_eval(t->ease, k);
    return t->value;
}

int tw_active(const tween *t, double now)
{
    return t->ms > 0 && (now - t->t0) * 1000.0 < t->ms + t->delay_ms;
}
