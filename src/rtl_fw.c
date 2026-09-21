/*
 * rtl_fw — firmware download for RTL8812AU (macOS, libusb).
 *
 * Ported from hal/rtl8812a/rtl8812a_hal_init.c:
 *   FirmwareDownload8812, _WriteFW_8812, _PageWrite_8812, _BlockWrite_8812,
 *   _FWDownloadEnable_8812, polling_fwdl_chksum, _FWFreeToGo8812, _8051Reset8812
 *
 * Flow: check header (shift by 32 bytes if needed) -> enable download ->
 * write firmware page by page (4 KB) in 196/8/1-byte blocks into the FIFO
 * (0x1000) -> poll checksum -> poll WINTINI_RDY.
 */
#include "rtl_usb.h"
#include <stdio.h>
#include <unistd.h>

#define BIT(n) (1u << (n))

/* Embedded NIC firmware (src/fw_8812a_nic.c). */
extern unsigned char array_mp_8812a_fw_nic[];
extern const unsigned int array_mp_8812a_fw_nic_len;

static void fwdl_enable(libusb_device_handle *h, int enable) {
    if (enable) {
        rtl_write8(h, REG_MCUFWDL,     rtl_read8(h, REG_MCUFWDL, NULL) | MCUFWDL_EN);
        rtl_write8(h, REG_MCUFWDL + 2, rtl_read8(h, REG_MCUFWDL + 2, NULL) & 0xf7); /* 8051 reset */
    } else {
        rtl_write8(h, REG_MCUFWDL,     rtl_read8(h, REG_MCUFWDL, NULL) & 0xfe);
    }
}

static void reset_8051(libusb_device_handle *h) {
    uint8_t t2;
    /* Reset MCU IO Wrapper (8812) */
    t2 = rtl_read8(h, REG_RSV_CTRL, NULL);     rtl_write8(h, REG_RSV_CTRL, t2 & ~BIT(1));
    t2 = rtl_read8(h, REG_RSV_CTRL + 1, NULL); rtl_write8(h, REG_RSV_CTRL + 1, t2 & ~BIT(3));
    uint8_t t = rtl_read8(h, REG_SYS_FUNC_EN + 1, NULL);
    rtl_write8(h, REG_SYS_FUNC_EN + 1, t & ~BIT(2));  /* hold 8051 in reset */
    /* Enable MCU IO Wrapper */
    t2 = rtl_read8(h, REG_RSV_CTRL, NULL);     rtl_write8(h, REG_RSV_CTRL, t2 & ~BIT(1));
    t2 = rtl_read8(h, REG_RSV_CTRL + 1, NULL); rtl_write8(h, REG_RSV_CTRL + 1, t2 | BIT(3));
    rtl_write8(h, REG_SYS_FUNC_EN + 1, t | BIT(2));   /* release 8051 */
}

static int block_write(libusb_device_handle *h, const uint8_t *buf, uint32_t size) {
    uint32_t b1 = FWDL_MAX_BLOCK, b2 = 8;
    uint32_t cnt1 = size / b1, rem1 = size % b1;
    uint32_t i, off, rc;

    for (i = 0; i < cnt1; i++) {
        rc = rtl_reg_write_block(h, (uint16_t)(FW_START_ADDRESS + i * b1), buf + i * b1, (uint16_t)b1);
        if (rc) return rc;
    }
    if (rem1) {
        off = cnt1 * b1;
        uint32_t cnt2 = rem1 / b2, rem2 = rem1 % b2;
        for (i = 0; i < cnt2; i++) {
            rc = rtl_reg_write_block(h, (uint16_t)(FW_START_ADDRESS + off + i * b2), buf + off + i * b2, (uint16_t)b2);
            if (rc) return rc;
        }
        if (rem2) {
            off = cnt1 * b1 + cnt2 * b2;
            for (i = 0; i < rem2; i++) {
                rc = rtl_write8(h, (uint16_t)(FW_START_ADDRESS + off + i), buf[off + i]);
                if (rc) return rc;
            }
        }
    }
    return 0;
}

static int page_write(libusb_device_handle *h, uint32_t page, const uint8_t *buf, uint32_t size) {
    uint8_t v = (uint8_t)((rtl_read8(h, REG_MCUFWDL + 2, NULL) & 0xF8) | (page & 0x07));
    int rc = rtl_write8(h, REG_MCUFWDL + 2, v);
    if (rc) return rc;
    return block_write(h, buf, size);
}

static int write_fw(libusb_device_handle *h, const uint8_t *buf, uint32_t size) {
    uint32_t pages = size / MAX_DLFW_PAGE_SIZE;
    uint32_t rem   = size % MAX_DLFW_PAGE_SIZE;
    uint32_t page, off, rc;
    for (page = 0; page < pages; page++) {
        off = page * MAX_DLFW_PAGE_SIZE;
        rc = page_write(h, page, buf + off, MAX_DLFW_PAGE_SIZE);
        if (rc) return rc;
    }
    if (rem) {
        off = pages * MAX_DLFW_PAGE_SIZE;
        rc = page_write(h, pages, buf + off, rem);
        if (rc) return rc;
    }
    return 0;
}

static int poll_flag32(libusb_device_handle *h, uint32_t mask, int tries) {
    for (int i = 0; i < tries; i++) {
        uint32_t v = rtl_read32(h, REG_MCUFWDL, NULL);
        if (v & mask) return 0;
        usleep(100);
    }
    return -1;
}

int rtl_fw_download(libusb_device_handle *h, int verbose) {
    const uint8_t *buf = array_mp_8812a_fw_nic;
    uint32_t len = array_mp_8812a_fw_nic_len;

    uint16_t sig = (uint16_t)(buf[0] | (buf[1] << 8));
    uint16_t ver = (uint16_t)(buf[4] | (buf[5] << 8));
    uint8_t  sub = buf[6];
    if (verbose) printf("  Firmware sig=0x%04x ver=%u.%u len=%u\n", sig, ver, sub, len);

    if (((sig & 0xFFF0) == 0x9500) || ((sig & 0xFFF0) == 0x2100)) {
        buf += 32; len -= 32;               /* strip the 32-byte header */
        if (verbose) printf("  Header erkannt -> Nutzlast %u Byte\n", len);
    }

    /* Is the 8051 already running RAM code? If so, reset it first. */
    if (rtl_read8(h, REG_MCUFWDL, NULL) & RAM_DL_SEL) {
        rtl_write8(h, REG_MCUFWDL, 0x00);
        reset_8051(h);
    }

    fwdl_enable(h, 1);
    /* clear stale checksum report (W1C) */
    rtl_write8(h, REG_MCUFWDL, rtl_read8(h, REG_MCUFWDL, NULL) | FWDL_CHKSUM_RPT);

    int rc = write_fw(h, buf, len);
    if (rc) { fwdl_enable(h, 0); if (verbose) printf("  Block-Write FEHLER: %d\n", rc); return rc; }

    rc = poll_flag32(h, FWDL_CHKSUM_RPT, 1000);
    fwdl_enable(h, 0);
    if (rc) { if (verbose) printf("  Checksum-Report TIMEOUT\n"); return 1; }
    if (verbose) printf("  Checksum OK\n");

    /* Free to go: set RDY, clear WINTINI, reset 8051, then poll. */
    uint32_t v = rtl_read32(h, REG_MCUFWDL, NULL);
    v |= MCUFWDL_RDY; v &= ~((uint32_t)WINTINI_RDY);
    rtl_write32(h, REG_MCUFWDL, v);
    reset_8051(h);

    rc = poll_flag32(h, WINTINI_RDY, 2000);
    if (rc) { if (verbose) printf("  WINTINI_RDY TIMEOUT\n"); return 2; }
    if (verbose) printf("  FW ready (WINTINI_RDY gesetzt)\n");
    return 0;
}

/* ---------------------------------------------------------------------
 * H2C_SET_PWR_MODE (hal/rtl8812a/rtl8812a_cmd.c rtl8812_set_FwPwrMode_cmd,
 * hal_com_h2c.h H2C_PWRMODE_LEN=7) -- tell the embedded firmware to stay in
 * PS_MODE_ACTIVE (no power save) and never sent anywhere in this codebase
 * until now. The firmware runs autonomously on the chip's own MCU; without
 * this explicit command it can default to its own idle-triggered power-save
 * behavior (setting the 802.11 PM bit in its own auto-generated frames),
 * independent of anything the host writes to registers. Symptom this
 * explained: beacons kept arriving (RX hardware fine) but unicast delivery
 * died for good the first time outbound traffic paused -- once the AP
 * believes we're in power-save, it buffers unicast for us and waits for a
 * PS-Poll/null-data wake we never send, exactly matching "pause once, dead
 * forever, even after resuming traffic."
 *
 * Mailbox protocol (fill_h2c_cmd_8812): REG_HMETFR (0x1CC) bit<n> = box n
 * still unread by firmware; write the first 3 payload bytes + command id
 * into REG_HMEBOX_0 (0x1D0, 4 bytes LE), the remaining 4 into
 * REG_HMEBOX_EXT0 (0x1F0, 4 bytes LE). We only ever send this one command,
 * once, so box 0 unconditionally is fine -- no round-robin bookkeeping
 * needed. */
#define REG_HMETFR       0x01CC
#define REG_HMEBOX_0     0x01D0
#define REG_HMEBOX_EXT0  0x01F0
#define H2C_SET_PWR_MODE 0x20

int rtl_fw_set_active_mode(libusb_device_handle *h, int verbose) {
    for (int i = 0; i < 100; i++) {
        if (!(rtl_read8(h, REG_HMETFR, NULL) & BIT(0))) break;
        usleep(1000);
    }
    uint8_t p[7] = {0};
    p[0] = 0x00;  /* Mode = PS_MODE_ACTIVE (0) */
    p[1] = 0x00;  /* RLBM=0, Smart_PS=0 -- irrelevant in Active mode */
    p[2] = 0x00;  /* BCN_PASS_TIME -- irrelevant in Active mode */
    p[3] = 0x00;  /* AllQueueUAPSD = 0 */
    p[4] = 0x0C;  /* PWR_STATE = AllON */
    p[5] = 0x00; p[6] = 0x00;

    uint32_t h2c_cmd = (uint32_t)H2C_SET_PWR_MODE | ((uint32_t)p[0]<<8) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<24);
    uint32_t h2c_cmd_ex = (uint32_t)p[3] | ((uint32_t)p[4]<<8) | ((uint32_t)p[5]<<16) | ((uint32_t)p[6]<<24);

    rtl_write32(h, REG_HMEBOX_EXT0, h2c_cmd_ex);
    int rc = rtl_write32(h, REG_HMEBOX_0, h2c_cmd);
    if (verbose) printf("[fw] H2C_SET_PWR_MODE (Active, kein Power-Save) gesendet (rc=%d)\n", rc);
    return rc;
}
