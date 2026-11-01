#pragma once
#include <libusb-1.0/libusb.h>
#include "defs.h"

int send_packet(libusb_device_handle *h, const uint8_t *pkt);
void make_start(uint8_t p[PKT_LEN]);
void make_end(uint8_t p[PKT_LEN]);
int set_fixed_mode(libusb_device_handle *h);
int set_color(libusb_device_handle *h, uint8_t r, uint8_t g, uint8_t b);
int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b);
int set_brightness(libusb_device_handle *h, int level);
