/*
 * rtl_usb — native USB register access to the RTL8812AU (macOS, libusb).
 *
 * Ported from os_dep/linux/usb_ops_linux.c of the aircrack-ng/rtl8812au driver:
 *   Register-Read  = bmRequestType 0xC0, bRequest 0x05, wValue=addr, wIndex=0
 *   Register-Write = bmRequestType 0x40, bRequest 0x05, wValue=addr, wIndex=0
 * Data is little-endian.
 */
#ifndef RTL_USB_H
#define RTL_USB_H

#include <stdint.h>
#include <libusb.h>

#define RTL_REALTEK_VID   0x0bda
#define RTL_VENDOR_READ   0xC0  /* REALTEK_USB_VENQT_READ  */
#define RTL_VENDOR_WRITE  0x40  /* REALTEK_USB_VENQT_WRITE */
#define RTL_VENDOR_REQ    0x05  /* REALTEK_USB_VENQT_CMD_REQ */
#define RTL_VENDOR_IDX    0x00  /* REALTEK_USB_VENQT_CMD_IDX */
#define RTL_CTRL_TIMEOUT  500   /* ms */

/* Important registers (from include/hal_com_reg.h). */
#define REG_SYS_CFG       0x00F0  /* 32-bit: chip version/vendor/cut */
#define REG_MACID         0x0610  /* 6 byte: MAC address (after efuse autoload) */
#define REG_EFUSE_CTRL    0x0030  /* 32-bit: efuse access (data/addr/flag) */

/* efuse (from include/rtl8812a_hal.h, include/hal_pg.h). */
#define EFUSE_MAP_LEN     512     /* logical map size (Jaguar/8812) */
#define EFUSE_PHYS_MAX    1024    /* physical upper bound for the scan */
#define EFUSE_MAC_OFFSET  0xD7    /* EEPROM_MAC_ADDR_8812AU: MAC in the map */

/* Firmware download (from include/hal_com_reg.h, rtl8812a_hal.h). */
#define REG_SYS_FUNC_EN   0x0002
#define REG_RSV_CTRL      0x001C
#define REG_MCUFWDL       0x0080
#define FW_START_ADDRESS  0x1000
#define MAX_DLFW_PAGE_SIZE 4096
#define FWDL_MAX_BLOCK    196     /* MAX_REG_BOLCK_SIZE (USB) */
#define MCUFWDL_EN        0x01    /* BIT0 */
#define MCUFWDL_RDY       0x02    /* BIT1 */
#define FWDL_CHKSUM_RPT   0x04    /* BIT2 */
#define WINTINI_RDY       0x40    /* BIT6 */
#define RAM_DL_SEL        0x80    /* BIT7 */

/* LED (from include/hal_com_reg.h, hal/rtl8812a/usb/rtl8812au_led.c). */
#define REG_LEDCFG2       0x004E

/* SYS_CFG bits. */
#define SYS_CFG_RTL_ID          (1u << 23) /* 1=test chip, 0=MP */
#define SYS_CFG_VENDOR_ID       (1u << 19) /* 1=UMC, 0=TSMC (8812) */
#define SYS_CFG_CHIP_VER_MASK   0x0000F000u
#define SYS_CFG_CHIP_VER_SHIFT  12

/* Finds and opens the first RTL8812AU (VID 0x0bda). Claims interface 0.
 * Returns 0 and sets *out_handle; otherwise a libusb error code (<0)
 * or LIBUSB_ERROR_NO_DEVICE if none is plugged in. *claimed tells whether the
 * interface claim succeeded (reads can sometimes be attempted without it). */
int rtl_open_first(libusb_context *ctx, libusb_device_handle **out_handle,
                   uint16_t *out_pid, int *claimed);

/* Register access. len is 1, 2 or 4. Returns: 0 ok, otherwise libusb error. */
int rtl_reg_read(libusb_device_handle *h, uint16_t addr, uint8_t *buf, uint16_t len);
int rtl_reg_write(libusb_device_handle *h, uint16_t addr, const uint8_t *buf, uint16_t len);
/* Block-Write (bis 256 Byte) fuer Firmware-Download via Vendor-Request. */
int rtl_reg_write_block(libusb_device_handle *h, uint16_t addr, const uint8_t *buf, uint16_t len);

/* Bequeme Breiten-Helfer (little-endian). Fehler -> Rueckgabe 0 bzw. via rc-Ptr. */
uint8_t  rtl_read8 (libusb_device_handle *h, uint16_t addr, int *rc);
uint16_t rtl_read16(libusb_device_handle *h, uint16_t addr, int *rc);
uint32_t rtl_read32(libusb_device_handle *h, uint16_t addr, int *rc);
int rtl_write8 (libusb_device_handle *h, uint16_t addr, uint8_t  val);
int rtl_write16(libusb_device_handle *h, uint16_t addr, uint16_t val);
int rtl_write32(libusb_device_handle *h, uint16_t addr, uint32_t val);

/* efuse: ein physisches Byte lesen bzw. die logische Map dekodieren.
 * map muss EFUSE_MAP_LEN gross sein. Rueckgabe 0 ok, sonst libusb-Fehler. */
int rtl_efuse_read_byte(libusb_device_handle *h, uint16_t addr, uint8_t *out);
int rtl_efuse_read_map(libusb_device_handle *h, uint8_t *map, int maplen);

/* Power-On (card enable): faehrt die aus dem Linux-HAL portierte
 * CARDEMU_TO_ACT-Sequenz. verbose!=0 druckt jeden Schritt.
 * Rueckgabe: 0 ok, <0 bei USB-Fehler, >0 = Nummer des fehlgeschlagenen
 * POLLING-Schritts (Chip-State-Machine nicht erreicht). */
int rtl_power_on(libusb_device_handle *h, int verbose);

/* Firmware-Download (NIC-Firmware, eingebettet). verbose!=0 druckt Fortschritt.
 * Rueckgabe: 0 ok, <0 USB-Fehler, 1 = Checksum-Timeout, 2 = WINTINI-Timeout.
 * Setzt eine erfolgreiche Power-On-Sequenz voraus. */
int rtl_fw_download(libusb_device_handle *h, int verbose);

/* Status-LED0 (SW-Control) an/aus. */
void rtl_led_on(libusb_device_handle *h);
void rtl_led_off(libusb_device_handle *h);

#endif /* RTL_USB_H */
