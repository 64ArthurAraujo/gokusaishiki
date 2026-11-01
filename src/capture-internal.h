/* Internal interfaces shared by the screen capture backends. */

#ifndef CAPTURE_INTERNAL_H
#define CAPTURE_INTERNAL_H

#include <stdint.h>
#include <signal.h>
#include "screen-color.h"

#define GRID_COLS 96
#define GRID_ROWS 54

/* ---- color histogram (dominant color algorithm) ---- */

typedef struct color_hist color_hist_t;

color_hist_t *hist_new(void);
void hist_free(color_hist_t *h);
void hist_reset(color_hist_t *h);
void hist_add(color_hist_t *h, uint8_t r, uint8_t g, uint8_t b);
int  hist_result(const color_hist_t *h, rgb_t *out);

/*
 * Sample a GRID_COLS x GRID_ROWS grid from a packed 32-bit-per-pixel frame
 * and add it to the histogram. r_off/g_off/b_off are the byte offsets of
 * each channel inside a pixel (e.g. BGRx: r=2, g=1, b=0).
 */
void hist_add_frame32(color_hist_t *h, const uint8_t *data,
                      int width, int height, int stride,
                      int r_off, int g_off, int b_off);

/* ---- backends ---- */

typedef struct {
    const char *name;
    void *(*open)(void);
    void (*close)(void *impl);
    int  (*dominant)(void *impl, rgb_t *out);
} capture_backend_t;

extern const capture_backend_t capture_backend_x11;
extern const capture_backend_t capture_backend_portal;

/* Set by screen_capture_set_abort_flag(); NULL if unused. */
extern volatile sig_atomic_t *g_capture_abort;
#define CAPTURE_ABORTED() (g_capture_abort && *g_capture_abort)

#endif /* CAPTURE_INTERNAL_H */
