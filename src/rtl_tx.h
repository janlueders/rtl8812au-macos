/*
 * rtl_tx — injection (TX) for RTL8812AU (macOS, libusb).
 *
 * Builds a 40-byte TX descriptor (with mandatory checksum) in front of the raw
 * 802.11 frame and sends both over a bulk-OUT endpoint.
 *
 * SAFETY: transmits at the moderate default TX power (index 0x12 from the
 * BB table), NOT at maximum. No PA overload risk.
 *
 * TXDESC fields from include/rtl8812a_xmit.h; checksum from
 * rtl8812a_cal_txdesc_chksum (XOR of the first 32 bytes).
 */
#ifndef RTL_TX_H
#define RTL_TX_H

#include <stdint.h>
#include <libusb.h>

#define RTL_TXDESC_SIZE   40
#define RTL_TX_EP_MGMT    0x02   /* high/mgmt queue -> first bulk-OUT */

/* Rate codes (hal_com.h). */
#define RTL_RATE_1M   0x00
#define RTL_RATE_6M   0x04
#define RTL_RATE_54M  0x0b

/* QSEL (hal_com.h). */
#define RTL_QSLT_MGNT 0x12
#define RTL_QSLT_BE   0x00
#define RTL_QSLT_VO   0x07

/* Inject a raw 802.11 frame. rate = RTL_RATE_*, qsel = RTL_QSLT_*,
 * ep = bulk-OUT endpoint. Returns 0 ok, otherwise a libusb error. */
int rtl_tx_inject(libusb_device_handle *h, const uint8_t *frame, int len,
                  uint8_t rate, uint8_t qsel, uint8_t ep);

#endif /* RTL_TX_H */
