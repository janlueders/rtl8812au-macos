
#include "rtl_chip.h"
#include "rtl_usb.h"
#include "rtl_mac.h"
#include "rtl_bb.h"
#include "rtl_rf.h"
#include "rtl_cal.h"
#include "rtl_txpwr.h"
#include "rtl_tx.h"
#include <stdio.h>

#define RND8(x) (((x) + 7) & ~7u)

/* Endpunkte (hier, damit rtl_chip.c nicht rtl_rx.h/rtl_tx.h braucht). */
#define RTL_RX_EP_DEFAULT 0x81  /* Bulk-IN  (RX) */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * RX-Deskriptor 8812A (include/rtl8812a_recv.h):
 *   dword0 [13:0] pkt_len, [19:16] drvinfo_size (x8), [25:24] shift
 *   dword2 [28]   rpt_sel (C2H/Report statt 802.11-Frame)
 */
static int rtl8812a_rx_parse_desc(const uint8_t *p, int avail, rtl_rx_sub *out) {
    if (avail <= 24) return -1;

    uint32_t d0 = le32(p);
    uint32_t d2 = le32(p + 8);

    uint32_t pkt_len = d0 & 0x3FFFu;
    uint32_t drv     = ((d0 >> 16) & 0x0Fu) * 8u;
    uint32_t shift   = (d0 >> 24) & 0x03u;

    if (pkt_len == 0) return -1;

    uint32_t hdr   = 24u + drv + shift;
    uint32_t total = hdr + pkt_len;
    if ((int)total > avail) return -1;

    uint32_t adv = RND8(total);
    if ((int)adv > avail) adv = (uint32_t)avail;  /* letzter Subframe */

    out->hdr_len   = hdr;
    out->pkt_len   = pkt_len;
    out->advance   = adv;
    out->is_report = (int)((d2 >> 28) & 0x01u);
    return 0;
}

static void rtl8812a_dbg_dump(libusb_device_handle *h) {
    int rc = 0;
    uint8_t  cr   = rtl_read8 (h, 0x0100, NULL);
    uint32_t rcr  = rtl_read32(h, 0x0608, NULL);
    uint32_t ch18 = rtl_rf_read(h, RTL_RF_PATH_A, 0x18, &rc);
    printf("[hal] Readback: CR=0x%02x  RCR=0x%08x  RF_A[0x18]=0x%05x (rc=%d)\n",
           cr, rcr, ch18, rc);
}

static const uint16_t rtl8812au_pids[] = {
    0x8812, 0x881a, 0x881b, 0x881c, 0x8813, 0x0000
};

const rtl_chip_ops rtl8812au_ops = {
    .name           = "RTL8812AU",
    .pids           = rtl8812au_pids,

    .power_on       = rtl_power_on,
    .fw_download    = rtl_fw_download,
    .fw_active_mode = rtl_fw_set_active_mode,
    .mac_init       = rtl_mac_init,
    .bb_init        = rtl_bb_init,
    .rf_init        = rtl_rf_init,
    .set_channel    = rtl_rf_set_channel,
    .set_monitor    = rtl_mac_set_monitor,
    .lck            = rtl_lck,
    .iqk            = rtl_iqk,
    .txpwr_apply    = rtl_txpwr_apply,
    .txpwr_readback = rtl_txpwr_readback,
    .led_on         = rtl_led_on,
    .led_off        = rtl_led_off,
    .dbg_dump       = rtl8812a_dbg_dump,

    .rx_ep          = RTL_RX_EP_DEFAULT,
    .tx_ep_mgmt     = RTL_TX_EP_MGMT,
    .rxdesc_size    = 24,
    .txdesc_size    = RTL_TXDESC_SIZE,

    .rx_parse_desc  = rtl8812a_rx_parse_desc,
    .tx_inject      = rtl_tx_inject,
};

const rtl_chip_ops *rtl_chip_default(void) { return &rtl8812au_ops; }

const rtl_chip_ops *rtl_chip_probe(uint16_t pid) {
    static const rtl_chip_ops *const table[] = { &rtl8812au_ops, NULL };
    for (int i = 0; table[i]; i++)
        for (const uint16_t *p = table[i]->pids; *p; p++)
            if (*p == pid) return table[i];
    return rtl_chip_default();
}