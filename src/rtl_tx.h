/*
 * rtl_tx — Injection (TX) fuer RTL8812AU (macOS, libusb).
 *
 * Baut einen 40-Byte-TX-Deskriptor (mit Pflicht-Pruefsumme) vor den rohen
 * 802.11-Frame und schickt beides ueber einen Bulk-OUT-Endpoint.
 *
 * SICHERHEIT: sendet auf der moderaten Standard-TX-Power (Index 0x12 aus der
 * BB-Tabelle), NICHT auf Maximum. Kein PA-Ueberlastungsrisiko.
 *
 * TXDESC-Felder aus include/rtl8812a_xmit.h; Checksumme aus
 * rtl8812a_cal_txdesc_chksum (XOR der ersten 32 Byte).
 */
#ifndef RTL_TX_H
#define RTL_TX_H

#include <stdint.h>
#include <libusb.h>

#define RTL_TXDESC_SIZE   40
#define RTL_TX_EP_MGMT    0x02   /* High-/Mgmt-Queue -> erster Bulk-OUT */

/* Ratencodes (hal_com.h). */
#define RTL_RATE_1M   0x00
#define RTL_RATE_6M   0x04
#define RTL_RATE_54M  0x0b

/* QSEL (hal_com.h). */
#define RTL_QSLT_MGNT 0x12
#define RTL_QSLT_BE   0x00
#define RTL_QSLT_VO   0x07

/* Einen rohen 802.11-Frame injizieren. rate = RTL_RATE_*, qsel = RTL_QSLT_*,
 * ep = Bulk-OUT-Endpoint. Rueckgabe 0 ok, sonst libusb-Fehler. */
int rtl_tx_inject(libusb_device_handle *h, const uint8_t *frame, int len,
                  uint8_t rate, uint8_t qsel, uint8_t ep);

#endif /* RTL_TX_H */
