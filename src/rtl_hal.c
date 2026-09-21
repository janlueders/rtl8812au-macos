/*
 * rtl_hal — Bring-up-Orchestrator ueber rtl_chip_ops.
 *
 * Enthaelt bewusst KEINEN 8812AU-spezifischen Code mehr: Registeradressen,
 * Tabellen und Debug-Dumps stecken in der ops-Tabelle (rtl_chip.c).
 */
#include "rtl_hal.h"
#include <stdio.h>

/* Pflichtschritt: fehlender Funktionszeiger = kaputte ops-Tabelle. */
#define STEP(fn, call, name) do {                                              \
    if (!(fn)) { printf("[hal] %s: nicht implementiert\n", name); return -1; } \
    if (verbose) printf("[hal] %s ...\n", name);                               \
    int _rc = (call);                                                          \
    if (_rc != 0) {                                                            \
        printf("[hal] %s FEHLGESCHLAGEN (rc=%d)\n", name, _rc);                \
        return _rc;                                                            \
    }                                                                          \
} while (0)

/* Best-effort: darf fehlen und darf fehlschlagen. */
#define TRY(fn, call) do { if (fn) { (call); } } while (0)

void rtl_dev_bind(rtl_dev *dev, libusb_device_handle *h, uint16_t pid) {
    dev->usb     = h;
    dev->pid     = pid;
    dev->ops     = rtl_chip_probe(pid);
    dev->channel = 0;
}

void rtl_dev_bind_auto(rtl_dev *dev, libusb_device_handle *h) {
    uint16_t pid = 0;
    struct libusb_device_descriptor d;
    libusb_device *dv = libusb_get_device(h);
    if (dv && libusb_get_device_descriptor(dv, &d) == 0)
        pid = d.idProduct;
    rtl_dev_bind(dev, h, pid);
}

int rtl_hal_init_dev(rtl_dev *dev, int channel, int with_cal, int verbose) {
    if (!dev || !dev->ops) { printf("[hal] keine ops gebunden\n"); return -1; }

    const rtl_chip_ops   *o = dev->ops;
    libusb_device_handle *h = dev->usb;

    if (verbose) printf("[hal] Chip: %s (0bda:%04x)\n", o->name, dev->pid);

    STEP(o->power_on,    o->power_on(h, verbose),       "Power-On");
    STEP(o->fw_download, o->fw_download(h, verbose),    "Firmware-Download");

    /* Firmware explizit in PS_MODE_ACTIVE halten: sonst kann ihre autonome
     * Power-Save nach einer TX-Pause still Unicast am AP puffern lassen --
     * ohne Deauth, den man mitbekommen wuerde. Darf fehlschlagen. */
    TRY(o->fw_active_mode, o->fw_active_mode(h, verbose));

    STEP(o->mac_init,    o->mac_init(h, verbose),       "MAC-Init");
    STEP(o->bb_init,     o->bb_init(h, verbose),        "BB-Init");
    STEP(o->rf_init,     o->rf_init(h, verbose),        "RF-Init");
    STEP(o->set_channel, o->set_channel(h, channel, 0), "Kanal setzen");

    /* Efuse-kalibrierte TX-Power: ohne das sendet der Chip mit seinem rohen
     * Post-Reset-Gain, weit unter Nennleistung. Ein Efuse-Lesefehler darf das
     * Bring-up nicht abbrechen. */
    TRY(o->txpwr_apply, o->txpwr_apply(h, channel, verbose));

    if (with_cal) {
        STEP(o->lck, o->lck(h, verbose), "LC-Kalibrierung");
        STEP(o->iqk, o->iqk(h, verbose), "IQ-Kalibrierung");
    }

    STEP(o->set_monitor, o->set_monitor(h, verbose), "Monitor-RCR");

    /* Pruefen, ob Kalibrierung oder Monitor-Setup die TX-AGC-Register
     * zwischendurch zurueckgesetzt haben. */
    TRY(o->txpwr_readback, o->txpwr_readback(h, "Ende rtl_hal_init_dev"));
    TRY(o->led_on, o->led_on(h));

    dev->channel = channel;

    if (verbose) {
        TRY(o->dbg_dump, o->dbg_dump(h));
        printf("[hal] Init komplett, Kanal %d, Monitor aktiv%s.\n",
               channel, with_cal ? " (kalibriert)" : " (RX-only, ohne Kalibrierung)");
    }
    return 0;
}

int rtl_hal_set_channel(rtl_dev *dev, int channel, int verbose) {
    if (!dev || !dev->ops || !dev->ops->set_channel) return -1;

    int rc = dev->ops->set_channel(dev->usb, channel, 0);
    if (rc != 0) return rc;

    /* Die TX-Power-Kalibrierung ist kanalabhaengig -- beim Hopping muss sie
     * mitgezogen werden, sonst gilt weiter der Wert des Init-Kanals. */
    TRY(dev->ops->txpwr_apply, dev->ops->txpwr_apply(dev->usb, channel, verbose));

    dev->channel = channel;
    return 0;
}

int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose) {
    rtl_dev dev;
    rtl_dev_bind_auto(&dev, h);
    return rtl_hal_init_dev(&dev, channel, with_cal, verbose);
}