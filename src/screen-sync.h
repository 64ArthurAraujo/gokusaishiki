#pragma once
#include <libusb-1.0/libusb.h>

int run_screen_sync(libusb_device_handle *h, int interval_ms);