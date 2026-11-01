#include <libusb-1.0/libusb.h>
#include <stdio.h>

int detach_kernel_driver(libusb_device_handle *h)
{
    int r = libusb_detach_kernel_driver(h, 1);
    if (r < 0)
        fprintf(stderr, "detach_kernel_driver: %s\n", libusb_error_name(r));
    return r;
}

int claim_interface(libusb_device_handle *h)
{
    int r = libusb_claim_interface(h, 1);
    if (r < 0)
        fprintf(stderr, "claim_interface(%d): %s\n", 1, libusb_error_name(r));
    return r;
}

int release_interface(libusb_device_handle *h)
{
    int r = libusb_release_interface(h, 1);
    if (r < 0)
        fprintf(stderr, "release_interface: %s\n", libusb_error_name(r));
}

int attach_driver(libusb_device_handle *h)
{
    int r = libusb_attach_kernel_driver(h, 1);
    if (r < 0)
        fprintf(stderr, "attach_kernel_driver: %s\n", libusb_error_name(r));
}