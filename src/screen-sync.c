#define _POSIX_C_SOURCE 200809L

#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <glib.h>

#include "defs.h"
#include "screen-color.h"
#include "color-smooth.h"
#include "ledctl.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int color_distance(rgb_t a, rgb_t b)
{
    return abs((int)a.r - b.r) + abs((int)a.g - b.g) + abs((int)a.b - b.b);
}

static bool same_color(rgb_t a, rgb_t b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(double ms)
{
    if (ms <= 0.0)
        return;

    time_t sec = (time_t)(ms / 1000.0);
    struct timespec delay = {
        .tv_sec = sec,
        .tv_nsec = (long)((ms - (double)sec * 1000.0) * 1e6),
    };
    nanosleep(&delay, NULL); /* interrupted by Ctrl-C, the loop then exits */
}

/*
 * Two things happen at different speeds:
 *   - every interval_ms the screen is sampled, which sets the TARGET color;
 *   - every 1000/rate_hz ms the displayed color takes a step toward the
 *     target (see smooth_step) and, if it changed, is sent to the keyboard.
 */
int run_screen_sync(libusb_device_handle *h, int interval_ms, int smooth_ms, int rate_hz)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal; /* no SA_RESTART: sleep is interrupted */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    screen_capture_set_abort_flag(&g_stop); /* Ctrl-C can interrupt a permission dialog */

    screen_capture_t *cap = screen_capture_open();
    if (!cap)
        return -1;
    printf("Capture backend: %s\n", screen_capture_backend_name(cap));

    const double tick_ms = 1000.0 / (double)rate_hz;
    const int deadband = smooth_ms > 0 ? TARGET_DEADBAND : COLOR_CHANGE_MIN;

    colorf_t current = {0, 0, 0};
    rgb_t target = {0, 0, 0};
    rgb_t sent = {0, 0, 0};
    bool have_target = false;
    bool have_sent = false;
    double next_sample = 0.0;
    double last_step = now_ms();
    int ret = 0;

    printf("Syncing keyboard to screen: sample every %d ms, smoothing %d ms, max %d updates/s (Ctrl-C to stop)\n",
           interval_ms, smooth_ms, rate_hz);

    while (!g_stop)
    {
        double t0 = now_ms();

        if (t0 >= next_sample)
        {
            rgb_t sample;

            if (screen_capture_dominant_color(cap, &sample) != 0)
            {
                ret = -1;
                break;
            }

            if (!have_target)
            {
                target = sample;
                smooth_reset(&current, sample); /* first color: no fade from black */
                have_target = true;
            }
            else if (color_distance(sample, target) >= deadband)
            {
                target = sample; /* small jitters are ignored */
            }

            next_sample = t0 + interval_ms;
        }

        rgb_t out = smooth_step(&current, target, t0 - last_step, smooth_ms);
        last_step = t0;

        if (!have_sent || !same_color(out, sent))
        {
            if (set_color(h, out.r, out.g, out.b) != 0)
            {
                ret = -1;
                break;
            }
            sent = out;
            have_sent = true;
            printf("\rColor: %02x%02x%02x ", out.r, out.g, out.b);
            fflush(stdout);
        }

        sleep_ms(tick_ms - (now_ms() - t0));
    }

    printf("\nStopped\n");
    screen_capture_close(cap);
    return ret;
}
