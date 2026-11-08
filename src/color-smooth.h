#pragma once
#include "screen-color.h"

/* Smoothed color kept in floating point so slow fades don't stall on rounding. */
typedef struct
{
    float r, g, b;
} colorf_t;

/* Jump straight to a color (no fade). Use for the very first sample. */
void smooth_reset(colorf_t *c, rgb_t target);

/*
 * Move c toward target for a step of dt_ms milliseconds and return the
 * color to display (rounded to bytes).
 *
 * smooth_ms is the time constant: after smooth_ms the color has covered
 * about half of the distance, and it is practically there after ~4x that.
 * The result depends on elapsed time (dt_ms), not on how often this is
 * called, so the fade looks the same at any update rate.
 * smooth_ms <= 0 disables smoothing (returns target immediately).
 */
rgb_t smooth_step(colorf_t *c, rgb_t target, double dt_ms, int smooth_ms);
