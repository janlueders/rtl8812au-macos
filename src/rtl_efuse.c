/*
 * rtl_efuse — efuse-Read fuer RTL8812AU (macOS, libusb).
 *
 * Low-Level-Bytelese ueber den kanonischen Realtek-Register-Poke an EFUSE_CTRL
 * (0x0030): das ist genau das, was halmac (rtw_halmac_read_physical_efuse)
 * intern tut. Danach Dekodierung der physischen efuse in die logische Map
 * (Header-Format mit Word-Enable, portiert aus rtl8812a_hal_init.c /
 * include/rtw_efuse.h).
 */
#include "rtl_usb.h"
#include <string.h>
#include <unistd.h>

/* Header-Makros (include/rtw_efuse.h). */
#define EXT_HEADER(h)          (((h) & 0x1F) == 0x0F)
#define GET_HDR_OFFSET_2_0(h)  (((h) & 0xE0) >> 5)
#define ALL_WORDS_DISABLED(w)  (((w) & 0x0F) == 0x0F)

/* Ein physisches efuse-Byte lesen.
 * EFUSE_CTRL(0x30)[7:0]=data, +1(0x31)=addr[7:0], +2(0x32)[1:0]=addr[9:8],
 * +3(0x33) bit7 = Busy/Valid-Flag (0 schreiben startet Read, 1 = fertig). */
int rtl_efuse_read_byte(libusb_device_handle *h, uint16_t addr, uint8_t *out) {
    int rc;
    rc = rtl_write8(h, REG_EFUSE_CTRL + 1, (uint8_t)(addr & 0xff));
    if (rc) return rc;
    uint8_t hi = rtl_read8(h, REG_EFUSE_CTRL + 2, &rc);
    if (rc) return rc;
    rc = rtl_write8(h, REG_EFUSE_CTRL + 2, (uint8_t)(((addr >> 8) & 0x03) | (hi & 0xFC)));
    if (rc) return rc;
    rc = rtl_write8(h, REG_EFUSE_CTRL + 3, 0x72); /* Read-Kommando, bit7=0 */
    if (rc) return rc;

    for (int i = 0; i < 1000; i++) {
        uint8_t flag = rtl_read8(h, REG_EFUSE_CTRL + 3, &rc);
        if (rc) return rc;
        if (flag & 0x80) {              /* Read fertig */
            *out = rtl_read8(h, REG_EFUSE_CTRL, &rc);
            return rc;
        }
        usleep(10);
    }
    return LIBUSB_ERROR_TIMEOUT;
}

/* Physische efuse in die logische Map dekodieren. map muss EFUSE_MAP_LEN gross
 * sein; unbeschriebene Stellen bleiben 0xFF. Rueckgabe 0 ok, sonst Fehler. */
int rtl_efuse_read_map(libusb_device_handle *h, uint8_t *map, int maplen) {
    memset(map, 0xFF, maplen);
    uint16_t addr = 0;
    uint8_t hdr;

    int rc = rtl_efuse_read_byte(h, addr++, &hdr);
    if (rc) return rc;
    if (hdr == 0xFF) return 0; /* efuse leer */

    while (hdr != 0xFF && addr < EFUSE_PHYS_MAX) {
        uint8_t offset, worden;

        if (EXT_HEADER(hdr)) {
            uint8_t off_2_0 = GET_HDR_OFFSET_2_0(hdr);
            uint8_t ext;
            rc = rtl_efuse_read_byte(h, addr++, &ext);
            if (rc) return rc;
            if (ext == 0xFF) break;
            if (ALL_WORDS_DISABLED(ext)) {
                rc = rtl_efuse_read_byte(h, addr++, &hdr); /* naechster Header */
                if (rc) return rc;
                continue;
            }
            offset = (uint8_t)(((ext & 0xF0) >> 1) | off_2_0);
            worden = ext & 0x0F;
        } else {
            offset = (hdr >> 4) & 0x0F;
            worden = hdr & 0x0F;
        }

        for (int i = 0; i < 4; i++) {
            if (!(worden & (1 << i))) {
                uint8_t d0, d1;
                rc = rtl_efuse_read_byte(h, addr++, &d0);
                if (rc) return rc;
                rc = rtl_efuse_read_byte(h, addr++, &d1);
                if (rc) return rc;
                int base = offset * 8 + i * 2;
                if (base + 1 < maplen) { map[base] = d0; map[base + 1] = d1; }
            }
        }

        rc = rtl_efuse_read_byte(h, addr++, &hdr);
        if (rc) return rc;
    }
    return 0;
}
