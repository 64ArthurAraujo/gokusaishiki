/*
 * screen_color - find the most prominent color on screen.
 *
 * Backends (chosen automatically):
 *   - "portal": Wayland/GNOME, via xdg-desktop-portal ScreenCast + PipeWire
 *   - "x11":    X11, via Xlib
 * Force one with the environment variable SHIKI_CAPTURE=x11|portal.
 *
 * Usage:
 *   screen_capture_t *cap = screen_capture_open();
 *   rgb_t c;
 *   if (cap && screen_capture_dominant_color(cap, &c) == 0)
 *       printf("%02x%02x%02x\n", c.r, c.g, c.b);
 *   screen_capture_close(cap);
 */

#ifndef SCREEN_COLOR_H
#define SCREEN_COLOR_H

#include <stdint.h>
#include <signal.h>

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef struct screen_capture screen_capture_t;

/*
 * Optional: a flag (set from a signal handler) that lets a long wait inside
 * screen_capture_open() (e.g. the Wayland permission dialog) be interrupted.
 */
void screen_capture_set_abort_flag(volatile sig_atomic_t *flag);

/* Open the best backend for this session. Returns NULL on failure. */
screen_capture_t *screen_capture_open(void);

/* Name of the backend in use ("x11" or "portal"). */
const char *screen_capture_backend_name(const screen_capture_t *cap);

/* Disconnect and free. Safe to call with NULL. */
void screen_capture_close(screen_capture_t *cap);

/*
 * Compute the most prominent color of the screen (latest frame).
 * Returns 0 on success, -1 on failure.
 */
int screen_capture_dominant_color(screen_capture_t *cap, rgb_t *out);

#endif /* SCREEN_COLOR_H */
