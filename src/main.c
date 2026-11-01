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

#include "ledctl.h"
#include "defs.h"
#include "libusb-routines.h"
#include "capture-internal.h"
#include "screen-color.h"

#define IFACE_N 1

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

static int run_screen_sync(libusb_device_handle *h, int interval_ms)
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

int set_keyboard_color(libusb_device_handle *h, uint8_t r, uint8_t g, uint8_t b)
{
    if (set_fixed_mode(h) != 0)
    {
        fprintf(stderr, "Failed to select fixed mode\n");
        return 1;
    }
    if (set_color(h, r, g, b) != 0)
    {
        fprintf(stderr, "Failed to set color\n");
        return 1;
    }

    return 0;
}

static gint interval = 250;

static GOptionEntry entries[] = {
    {"interval", 'i', 0, G_OPTION_ARG_INT, &interval, "Interval in milliseconds that the keyboard's color is updated", "N"},
    G_OPTION_ENTRY_NULL};

int main(int argc, char **argv)
{
    GError *error = NULL;

    GOptionContext *context = g_option_context_new("- Dynamic keyboard RGB based on the screen's content");

    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_print("option parsing failed: %s\n", error->message);
        return 1;
    }

    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;

    if (libusb_init_context(&ctx, NULL, 0) != 0)
    {
        fprintf(stderr, "Failed to initialize libusb context!\n");
        goto cleanup;
    }

    handle = libusb_open_device_with_vid_pid(ctx, VENDOR_RED_DRAGON, DEVICE_MITRA);

    if (!handle)
    {
        fprintf(stderr, "Failed to initialize keyboard handle, make sure the keyboard is connected!\n");
        goto cleanup;
    }

    if (detach_kernel_driver(handle) < 0)
        goto cleanup;

    if (claim_interface(handle) < 0)
        goto cleanup;

    if (run_screen_sync(handle, interval) != 0)
    {
        fprintf(stderr, "Screen sync failed\n");
        goto cleanup;
    }

cleanup:
    release_interface(handle);
    attach_driver(handle);

    if (handle)
        libusb_close(handle);

    libusb_exit(ctx);
    return 0;
}