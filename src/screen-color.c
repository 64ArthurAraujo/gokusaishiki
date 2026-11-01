/*
 * screen_color.c - dominant color algorithm + backend selection.
 *
 * Algorithm:
 *   1. Sample a GRID_COLS x GRID_ROWS grid of pixels (not every pixel).
 *   2. Put each sample into a histogram of 16x16x16 color bins, giving
 *      vivid pixels more weight than gray ones, and ignoring near-black.
 *   3. Pick the bin with the highest weight and return the average color
 *      of the pixels that fell into it.
 *   4. If every sample was near-black, return the plain average instead.
 */

#include "capture-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BINS        4096          /* 16 levels per channel */
#define DARK_LIMIT  20            /* ignore pixels darker than this */

typedef struct {
    uint32_t score;               /* weighted vote */
    uint32_t count;
    uint32_t r, g, b;             /* sums, to compute the bin's average */
} bin_t;

struct color_hist {
    bin_t    bins[BINS];
    uint64_t all_r, all_g, all_b, all_n;
};

volatile sig_atomic_t *g_capture_abort = NULL;

color_hist_t *hist_new(void)
{
    return calloc(1, sizeof(color_hist_t));
}

void hist_free(color_hist_t *h)
{
    free(h);
}

void hist_reset(color_hist_t *h)
{
    memset(h, 0, sizeof(*h));
}

void hist_add(color_hist_t *h, uint8_t r, uint8_t g, uint8_t b)
{
    h->all_r += r; h->all_g += g; h->all_b += b; h->all_n++;

    uint8_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (max < DARK_LIMIT)
        return;

    /* vivid colors vote more than gray ones */
    uint32_t weight = 16u + (uint32_t)(max - min);

    bin_t *bin = &h->bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
    bin->score += weight;
    bin->count++;
    bin->r += r; bin->g += g; bin->b += b;
}

int hist_result(const color_hist_t *h, rgb_t *out)
{
    const bin_t *best = NULL;

    for (int i = 0; i < BINS; i++) {
        if (h->bins[i].count && (!best || h->bins[i].score > best->score))
            best = &h->bins[i];
    }

    if (best) {
        out->r = (uint8_t)(best->r / best->count);
        out->g = (uint8_t)(best->g / best->count);
        out->b = (uint8_t)(best->b / best->count);
        return 0;
    }
    if (h->all_n) {                          /* whole screen is nearly black */
        out->r = (uint8_t)(h->all_r / h->all_n);
        out->g = (uint8_t)(h->all_g / h->all_n);
        out->b = (uint8_t)(h->all_b / h->all_n);
        return 0;
    }
    return -1;
}

void hist_add_frame32(color_hist_t *h, const uint8_t *data,
                      int width, int height, int stride,
                      int r_off, int g_off, int b_off)
{
    for (int row = 0; row < GRID_ROWS; row++) {
        int y = (row * height) / GRID_ROWS + height / (2 * GRID_ROWS);
        const uint8_t *line = data + (size_t)y * (size_t)stride;

        for (int col = 0; col < GRID_COLS; col++) {
            int x = (col * width) / GRID_COLS + width / (2 * GRID_COLS);
            const uint8_t *px = line + (size_t)x * 4;

            hist_add(h, px[r_off], px[g_off], px[b_off]);
        }
    }
}

/* ---------- backend selection ---------- */

struct screen_capture {
    const capture_backend_t *be;
    void *impl;
};

void screen_capture_set_abort_flag(volatile sig_atomic_t *flag)
{
    g_capture_abort = flag;
}

static const capture_backend_t *choose_backend(void)
{
    const char *force = getenv("KBD_RGB_CAPTURE");
    if (force) {
        // if (strcmp(force, "x11") == 0)    return &capture_backend_x11;
        if (strcmp(force, "portal") == 0) return &capture_backend_portal;
        fprintf(stderr, "Unknown KBD_RGB_CAPTURE='%s' (use x11 or portal)\n", force);
    }

    const char *session = getenv("XDG_SESSION_TYPE");
    if ((session && strcmp(session, "wayland") == 0) || getenv("WAYLAND_DISPLAY"))
        return &capture_backend_portal;

    return &capture_backend_portal;
}

screen_capture_t *screen_capture_open(void)
{
    screen_capture_t *cap = calloc(1, sizeof(*cap));
    if (!cap)
        return NULL;

    cap->be = choose_backend();
    cap->impl = cap->be->open();
    if (!cap->impl) {
        free(cap);
        return NULL;
    }
    return cap;
}

const char *screen_capture_backend_name(const screen_capture_t *cap)
{
    return cap ? cap->be->name : "none";
}

void screen_capture_close(screen_capture_t *cap)
{
    if (!cap)
        return;
    cap->be->close(cap->impl);
    free(cap);
}

int screen_capture_dominant_color(screen_capture_t *cap, rgb_t *out)
{
    if (!cap || !out)
        return -1;
    return cap->be->dominant(cap->impl, out);
}
