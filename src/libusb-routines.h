#pragma once
#include <libusb-1.0/libusb.h>

int detach_kernel_driver(libusb_device_handle *h);
int claim_interface(libusb_device_handle *h);
int release_interface(libusb_device_handle *h);
int attach_driver(libusb_device_handle *h);
