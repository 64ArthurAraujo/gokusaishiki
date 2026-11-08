#pragma once
#include <libusb-1.0/libusb.h>

/*
 * interval_ms: how often the screen is sampled
 * smooth_ms:   fade time constant in ms (0 = jump straight to each new color)
 * rate_hz:     maximum number of color updates sent to the keyboard per second
 */
int run_screen_sync(libusb_device_handle *h, int interval_ms, int smooth_ms, int rate_hz);
