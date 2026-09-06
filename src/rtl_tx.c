#include "rtl_tx.h"
#include <string.h>

/* SET_BITS_TO_LE_4BYTE-Aequivalent: nbits ab bitoff im LE-DWord bei byteoff. */
static void set_bits(uint8_t *d, int byteoff, int bitoff, int nbits, uint32_t val) {
    uint32_t cur = (uint32_t)d[byteoff] | ((uint32_t)d[byteoff+1] << 8) |
                   ((uint32_t)d[byteoff+2] << 16) | ((uint32_t)d[byteoff+3] << 24);
    uint32_t mask = ((nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1)) << bitoff;
    cur = (cur & ~mask) | ((val << bitoff) & mask);
    d[byteoff]   = cur & 0xff;
    d[byteoff+1] = (cur >> 8) & 0xff;
    d[byteoff+2] = (cur >> 16) & 0xff;
    d[byteoff+3] = (cur >> 24) & 0xff;
}

/* rtl8812a_cal_txdesc_chksum: XOR der ersten 32 Byte (16 u16), Feld vorher 0. */
static void tx_checksum(uint8_t *desc) {
    set_bits(desc, 28, 0, 16, 0);
    uint16_t cs = 0;
    for (int i = 0; i < 16; i++)
        cs ^= (uint16_t)(desc[2*i] | (desc[2*i+1] << 8));
    set_bits(desc, 28, 0, 16, cs);
}

int rtl_tx_inject(libusb_device_handle *h, const uint8_t *frame, int len,
                  uint8_t rate, uint8_t qsel, uint8_t ep) {
    if (len <= 0 || len > 4096) return LIBUSB_ERROR_INVALID_PARAM;

    uint8_t pkt[RTL_TXDESC_SIZE + 4096];
    uint8_t *desc = pkt;
    memset(desc, 0, RTL_TXDESC_SIZE);

    /* dword0: pkt_size, offset(=desc len), first/last seg, own */
    set_bits(desc, 0, 0, 16, (uint32_t)len);        /* PKT_SIZE */
    set_bits(desc, 0, 16, 8, RTL_TXDESC_SIZE);      /* OFFSET */
    set_bits(desc, 0, 26, 1, 1);                    /* LAST_SEG */
    set_bits(desc, 0, 27, 1, 1);                    /* FIRST_SEG */
    set_bits(desc, 0, 31, 1, 1);                    /* OWN */
    /* dword1(+4): macid, queue_sel, rate_id */
    set_bits(desc, 4, 8, 5, qsel);                  /* QUEUE_SEL */
    set_bits(desc, 4, 16, 5, (rate <= 0x03) ? 1u : 2u); /* RATE_ID: CCK(B)=1, OFDM(G)=2 */
    /* dword3(+12): use_rate, disable_fb */
    set_bits(desc, 12, 8, 1, 1);                    /* USE_RATE */
    set_bits(desc, 12, 10, 1, 1);                   /* DISABLE_FB */
    /* dword4(+16): tx_rate */
    set_bits(desc, 16, 0, 7, rate);                 /* TX_RATE */
    /* dword8(+32): hwseq_en -> Hardware vergibt Sequenznummer */
    set_bits(desc, 32, 15, 1, 1);                   /* HWSEQ_EN */

    tx_checksum(desc);

    memcpy(pkt + RTL_TXDESC_SIZE, frame, len);

    int total = RTL_TXDESC_SIZE + len;
    int sent = 0;
    int rc = libusb_bulk_transfer(h, ep, pkt, total, &sent, 500);
    if (rc != 0) return rc;
    return (sent == total) ? 0 : LIBUSB_ERROR_IO;
}
