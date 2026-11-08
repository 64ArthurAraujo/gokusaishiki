#include "color-smooth.h"

static float absf(float x)
{
    return x < 0.0f ? -x : x;
}

static uint8_t to_byte(float v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 255.0f)
        return 255;
    return (uint8_t)(v + 0.5f);
}

void smooth_reset(colorf_t *c, rgb_t target)
{
    c->r = target.r;
    c->g = target.g;
    c->b = target.b;
}

rgb_t smooth_step(colorf_t *c, rgb_t target, double dt_ms, int smooth_ms)
{
    if (smooth_ms <= 0)
    {
        smooth_reset(c, target);
    }
    else if (dt_ms > 0.0)
    {
        /* first-order low-pass filter: alpha = dt / (tau + dt) */
        float alpha = (float)(dt_ms / ((double)smooth_ms + dt_ms));

        c->r += ((float)target.r - c->r) * alpha;
        c->g += ((float)target.g - c->g) * alpha;
        c->b += ((float)target.b - c->b) * alpha;

        /* close enough: land exactly on the target so the fade ends */
        if (absf((float)target.r - c->r) < 1.0f &&
            absf((float)target.g - c->g) < 1.0f &&
            absf((float)target.b - c->b) < 1.0f)
            smooth_reset(c, target);
    }

    rgb_t out = {to_byte(c->r), to_byte(c->g), to_byte(c->b)};
    return out;
}
