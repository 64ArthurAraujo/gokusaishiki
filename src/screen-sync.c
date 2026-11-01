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

int run_screen_sync(libusb_device_handle *h, int interval_ms)
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

    struct timespec delay = {
        .tv_sec = interval_ms / 1000,
        .tv_nsec = (long)(interval_ms % 1000) * 1000000L,
    };

    rgb_t last = {0, 0, 0};
    bool have_last = false;
    int ret = 0;

    printf("Syncing keyboard to screen every %d ms (Ctrl-C to stop)\n",
           interval_ms);

    while (!g_stop)
    {
        rgb_t now;

        if (screen_capture_dominant_color(cap, &now) != 0)
        {
            ret = -1;
            break;
        }

        if (!have_last || color_distance(now, last) >= COLOR_CHANGE_MIN)
        {
            if (set_color(h, now.r, now.g, now.b) != 0)
            {
                ret = -1;
                break;
            }
            last = now;
            have_last = true;
            printf("\rColor: %02x%02x%02x ", now.r, now.g, now.b);
            fflush(stdout);
        }

        nanosleep(&delay, NULL);
    }

    printf("\nStopped\n");
    screen_capture_close(cap);
    return ret;
}