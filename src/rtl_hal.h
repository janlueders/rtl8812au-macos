/*
 * rtl_hal — orchestrator: full bring-up of the RTL8812AU to monitor mode.
 *
 * Threads the individual modules together in the correct order:
 *   Power-On -> Firmware -> MAC-Init -> BB-Init -> RF-Init -> LCK -> IQK
 *   -> set channel -> monitor RCR -> LED on.
 */
#ifndef RTL_HAL_H
#define RTL_HAL_H

#include <libusb.h>

/* Full bring-up including monitor configuration on 'channel' (20 MHz).
 * with_cal=0: pure RX bring-up without internal TX (no PA risk).
 * with_cal=1: additionally LCK+IQK (for injection quality).
 * Returns 0 ok, otherwise != 0 (the failed step is printed). */
int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose);

#endif /* RTL_HAL_H */
