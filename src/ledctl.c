#include <stdio.h>
#include <string.h>

#include "ledctl.h"

int send_packet(libusb_device_handle *h, const uint8_t *pkt)
{
    uint8_t buf[PKT_LEN];
    uint8_t out[PKT_LEN];
    int transferred = 0;

    memcpy(out, pkt, PKT_LEN);

    int rc = libusb_control_transfer(h, CTRL_REQTYPE, CTRL_REQUEST, CTRL_VALUE, CTRL_INDEX, out, PKT_LEN, TIMEOUT_MS);
    if (rc != PKT_LEN)
    {
        fprintf(stderr, "control transfer failed: %s\n", rc < 0 ? libusb_error_name(rc) : "short write");
        return -1;
    }

    rc = libusb_interrupt_transfer(h, EP_ACK_IN, buf, PKT_LEN, &transferred, TIMEOUT_MS);
    if (rc < 0)
    {
        fprintf(stderr, "reading reply from 0x%02x failed: %s\n", EP_ACK_IN, libusb_error_name(rc));
        return -1;
    }
    return 0;
}

void make_start(uint8_t p[PKT_LEN])
{
    memset(p, 0, PKT_LEN);
    p[0] = 0x04;
    p[1] = 0x01;
    p[3] = 0x01;
}

void make_end(uint8_t p[PKT_LEN])
{
    memset(p, 0, PKT_LEN);
    p[0] = 0x04;
    p[1] = 0x02;
    p[3] = 0x02;
}

int set_fixed_mode(libusb_device_handle *h)
{
    uint8_t start[PKT_LEN], mode[PKT_LEN], end[PKT_LEN];

    make_start(start);
    make_end(end);

    memset(mode, 0, PKT_LEN);
    mode[0] = 0x04;
    mode[1] = 0x0d;
    mode[3] = 0x06;
    mode[4] = 0x01;
    mode[8] = MODE_FIXED;

    if (send_packet(h, start))
        return -1;
    if (send_packet(h, mode))
        return -1;
    return send_packet(h, end);
}

int set_color(libusb_device_handle *h, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t start[PKT_LEN], hdr[PKT_LEN], data[PKT_LEN], end[PKT_LEN];

    make_start(start);
    make_end(end);

    memset(hdr, 0, PKT_LEN);
    hdr[0] = 0x04;
    hdr[1] = 0x0b;
    hdr[3] = 0x06;
    hdr[4] = 0x01;
    hdr[5] = 0x04;

    memset(data, 0, PKT_LEN);
    data[0] = 0x04;
    data[2] = 0x02;
    data[3] = 0x06;
    data[4] = 0x03;
    data[5] = 0x05;
    data[8] = r;
    data[9] = g;
    data[10] = b;

    if (send_packet(h, start))
        return -1;
    if (send_packet(h, hdr))
        return -1;
    if (send_packet(h, data))
        return -1;
    return send_packet(h, end);
}

int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned int rr, gg, bb;

    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6)
        return -1;
    if (sscanf(s, "%2x%2x%2x", &rr, &gg, &bb) != 3)
        return -1;

    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
    return 0;
}

int set_brightness(libusb_device_handle *h, int level)
{
    uint8_t start[PKT_LEN], br[PKT_LEN], end[PKT_LEN];

    if (level < BRIGHTNESS_MIN || level > BRIGHTNESS_MAX)
    {
        fprintf(stderr, "Brightness must be between %d..%d\n", BRIGHTNESS_MIN, BRIGHTNESS_MAX);
        return -1;
    }

    make_start(start);
    make_end(end);

    memset(br, 0, PKT_LEN);
    br[0] = 0x04;
    br[1] = (uint8_t)(0x08 + level);
    br[3] = 0x06;
    br[4] = 0x01;
    br[5] = 0x01;
    br[8] = (uint8_t)level;

    if (send_packet(h, start))
        return -1;
    if (send_packet(h, br))
        return -1;
    return send_packet(h, end);
}