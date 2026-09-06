/*
 * rtl_hal — full bring-up (orchestrator). See rtl_hal.h.
 *
 * Order follows the Linux rtl8812au_hal_init: power-on, firmware, MAC, BB,
 * RF, channel, (optional calibration), monitor RCR.
 *
 * with_cal=0 brings up RX ONLY (no internal TX, no PA risk) — for
 * safe initial monitor operation. with_cal=1 adds LCK+IQK (improves
 * TX/RX quality, needed for clean injection).
 */
#include "rtl_hal.h"
#include "rtl_usb.h"
#include "rtl_mac.h"
#include "rtl_bb.h"
#include "rtl_rf.h"
#include "rtl_cal.h"
#include <stdio.h>

#define STEP(call, name) do { \
    if (verbose) printf("[hal] %s ...\n", name); \
    int _rc = (call); \
    if (_rc != 0) { printf("[hal] %s FEHLGESCHLAGEN (rc=%d)\n", name, _rc); return _rc; } \
} while (0)

int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose) {
    STEP(rtl_power_on(h, verbose),           "Power-On");
    STEP(rtl_fw_download(h, verbose),         "Firmware-Download");
    STEP(rtl_mac_init(h, verbose),            "MAC-Init");
    STEP(rtl_bb_init(h, verbose),             "BB-Init");
    STEP(rtl_rf_init(h, verbose),             "RF-Init");
    STEP(rtl_rf_set_channel(h, channel, 0),   "Kanal setzen");
    if (with_cal) {
        STEP(rtl_lck(h, verbose),             "LC-Kalibrierung");
        STEP(rtl_iqk(h, verbose),             "IQ-Kalibrierung");
    }
    STEP(rtl_mac_set_monitor(h, verbose),     "Monitor-RCR");

    rtl_led_on(h);   /* LED on as soon as the chip is initialized (LEDCFG0) */

    /* Software proof instead of LED: read back the register state. */
    if (verbose) {
        int rc = 0;
        uint8_t  cr  = rtl_read8(h, 0x0100, NULL);        /* REG_CR */
        uint32_t rcr = rtl_read32(h, 0x0608, NULL);       /* REG_RCR */
        uint32_t ch18 = rtl_rf_read(h, 0, 0x18, &rc);     /* RF PathA channel reg */
        printf("[hal] Readback: CR=0x%02x  RCR=0x%08x  RF_A[0x18]=0x%05x (rc=%d)\n",
               cr, rcr, ch18, rc);
        printf("[hal] Init komplett, Kanal %d, Monitor aktiv%s.\n",
               channel, with_cal ? " (kalibriert)" : " (RX-only, ohne Kalibrierung)");
    }
    return 0;
}
