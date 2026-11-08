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
#include "screen-sync.h"

#define IFACE_N 1

static gint interval = 250;
static gint smooth_ms = SMOOTH_DEFAULT_MS;
static gint rate_hz = RATE_DEFAULT_HZ;

static GOptionEntry entries[] = {
    {"interval", 'i', 0, G_OPTION_ARG_INT, &interval, "Interval in milliseconds that the keyboard's color is updated", "250"},
    {"smooth", 's', 0, G_OPTION_ARG_INT, &smooth_ms, "Color fade time constant in milliseconds, 0 disables smoothing", "300"},
    {"rate", 'r', 0, G_OPTION_ARG_INT, &rate_hz, "Maximum number of color updates sent to the keyboard per second", "20"},
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

    if (smooth_ms < 0 || rate_hz < RATE_MIN_HZ || rate_hz > RATE_MAX_HZ)
    {
        fprintf(stderr, "Invalid options: --smooth must be >= 0 and --rate must be %d..%d\n", RATE_MIN_HZ, RATE_MAX_HZ);
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

    if (run_screen_sync(handle, interval, smooth_ms, rate_hz) != 0)
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