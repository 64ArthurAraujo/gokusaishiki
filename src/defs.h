#pragma once

#define PKT_LEN        64
#define CTRL_REQTYPE   0x21      /* host->device, class, interface */
#define CTRL_REQUEST   0x09      /* SET_REPORT */
#define CTRL_VALUE     0x0204
#define CTRL_INDEX     0x0001    /* interface 1 */
#define EP_ACK_IN      0x82
#define TIMEOUT_MS     1000
 
#define MODE_FIXED     0x06      /* mode id used by the keyboard */

#define BRIGHTNESS_MIN 0
#define BRIGHTNESS_MAX 5  

#define VENDOR_RED_DRAGON 0x0c45
#define DEVICE_MITRA      0x5004

#define COLOR_CHANGE_MIN   24    /* min change (sum of |dR|+|dG|+|dB|) to resend */
#define INTERVAL_DEFAULT  500    /* ms between screen samples */
#define INTERVAL_MIN      200
