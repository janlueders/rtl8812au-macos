/*
 * rtl_hal — volle Inbetriebnahme (Orchestrator). Siehe rtl_hal.h.
 *
 * Reihenfolge nach dem Linux-rtl8812au_hal_init: Power-On, Firmware, MAC, BB,
 * RF, Kalibrierung (LCK, IQK), Kanal, Monitor-RCR, LED.
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

int rtl_hal_full_init(libusb_device_handle *h, int channel, int verbose) {
    STEP(rtl_power_on(h, verbose),          "Power-On");
    STEP(rtl_fw_download(h, verbose),        "Firmware-Download");
    STEP(rtl_mac_init(h, verbose),           "MAC-Init");
    STEP(rtl_bb_init(h, verbose),            "BB-Init");
    STEP(rtl_rf_init(h, verbose),            "RF-Init");
    STEP(rtl_lck(h, verbose),                "LC-Kalibrierung");
    STEP(rtl_iqk(h, verbose),                "IQ-Kalibrierung");
    STEP(rtl_rf_set_channel(h, channel, 0),  "Kanal setzen");
    STEP(rtl_mac_set_monitor(h, verbose),    "Monitor-RCR");
    rtl_led_on(h);
    if (verbose) printf("[hal] Init komplett, Kanal %d, Monitor aktiv, LED an.\n", channel);
    return 0;
}
