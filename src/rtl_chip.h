/*
 * rtl_chip — Chip-Abstraktion (ops table).
 *
 * Buendelt alle chip-spezifischen Einsprungpunkte hinter einer Tabelle, damit
 * rtl_hal / rtl_rx / rtl_tx keinen 8812AU-spezifischen Code mehr enthalten.
 * Ein weiterer Chip (8811AU/8821AU) braucht dann nur eine zweite Instanz
 * von rtl_chip_ops plus seine eigenen Tabellenmodule.
 */
#ifndef RTL_CHIP_H
#define RTL_CHIP_H

#include <stdint.h>
#include <libusb.h>

/* Ergebnis des Zerlegens EINES aggregierten USB-RX-Subframes. */
typedef struct {
    uint32_t hdr_len;    /* Bytes vor dem 802.11-Frame (Desc + drvinfo + shift) */
    uint32_t pkt_len;    /* Laenge des 802.11-Frames                            */
    uint32_t advance;    /* Bytes bis zum naechsten Subframe (bereits 8-aligned) */
    int      is_report;  /* !=0 -> C2H/Report, kein 802.11-Frame                 */
} rtl_rx_sub;

typedef struct rtl_chip_ops {
    const char     *name;
    const uint16_t *pids;        /* 0-terminierte USB-PID-Liste */

    /* --- Bring-up (Reihenfolge legt rtl_hal fest, nicht der Chip) --- */
    int  (*power_on)      (libusb_device_handle *h, int verbose);
    int  (*fw_download)   (libusb_device_handle *h, int verbose);
    int  (*fw_active_mode)(libusb_device_handle *h, int verbose);
    int  (*mac_init)      (libusb_device_handle *h, int verbose);
    int  (*bb_init)       (libusb_device_handle *h, int verbose);
    int  (*rf_init)       (libusb_device_handle *h, int verbose);
    int  (*set_channel)   (libusb_device_handle *h, int ch, int bw);
    int  (*set_monitor)   (libusb_device_handle *h, int verbose);
    int  (*lck)           (libusb_device_handle *h, int verbose);
    int  (*iqk)           (libusb_device_handle *h, int verbose);
    int  (*txpwr_apply)   (libusb_device_handle *h, int ch, int verbose);
    void (*txpwr_readback)(libusb_device_handle *h, const char *label);
    void (*led_on)        (libusb_device_handle *h);
    void (*led_off)       (libusb_device_handle *h);
    void (*dbg_dump)      (libusb_device_handle *h);   /* optional, darf NULL sein */

    /* --- Datenpfad-Geometrie --- */
    uint8_t  rx_ep;
    uint8_t  tx_ep_mgmt;
    uint16_t rxdesc_size;
    uint16_t txdesc_size;

    /* Zerlegt den Deskriptor an 'p'. 0 = ok, <0 = unbrauchbar/abbrechen. */
    int (*rx_parse_desc)(const uint8_t *p, int avail, rtl_rx_sub *out);

    int (*tx_inject)(libusb_device_handle *h, const uint8_t *frame, int len,
                     uint8_t rate, uint8_t qsel, uint8_t ep);
} rtl_chip_ops;

/* PID -> ops. Unbekannte PID liefert rtl_chip_default() (Verhalten wie bisher). */
const rtl_chip_ops *rtl_chip_probe(uint16_t pid);
const rtl_chip_ops *rtl_chip_default(void);

extern const rtl_chip_ops rtl8812au_ops;

#endif /* RTL_CHIP_H */