/*
 * rtl_usb — native USB-Register-Zugriff auf den RTL8812AU (macOS, libusb).
 *
 * Portiert aus os_dep/linux/usb_ops_linux.c des aircrack-ng/rtl8812au-Treibers:
 *   Register-Read  = bmRequestType 0xC0, bRequest 0x05, wValue=addr, wIndex=0
 *   Register-Write = bmRequestType 0x40, bRequest 0x05, wValue=addr, wIndex=0
 * Daten sind little-endian.
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

/* Wichtige Register (aus include/hal_com_reg.h). */
#define REG_SYS_CFG       0x00F0  /* 32-bit: Chip-Version/Vendor/Cut */
#define REG_MACID         0x0610  /* 6 byte: MAC-Adresse (nach efuse-Autoload) */
#define REG_EFUSE_CTRL    0x0030  /* 32-bit: efuse-Zugriff (data/addr/flag) */

/* efuse (aus include/rtl8812a_hal.h, include/hal_pg.h). */
#define EFUSE_MAP_LEN     512     /* logische Map-Groesse (Jaguar/8812) */
#define EFUSE_PHYS_MAX    1024    /* physische Obergrenze fuer den Scan */
#define EFUSE_MAC_OFFSET  0xD7    /* EEPROM_MAC_ADDR_8812AU: MAC in der Map */

/* SYS_CFG-Bits. */
#define SYS_CFG_RTL_ID          (1u << 23) /* 1=Test-Chip, 0=MP */
#define SYS_CFG_VENDOR_ID       (1u << 19) /* 1=UMC, 0=TSMC (8812) */
#define SYS_CFG_CHIP_VER_MASK   0x0000F000u
#define SYS_CFG_CHIP_VER_SHIFT  12

/* Findet und oeffnet den ersten RTL8812AU (VID 0x0bda). Claimt Interface 0.
 * Gibt 0 zurueck und setzt *out_handle; sonst einen libusb-Fehlercode (<0)
 * oder LIBUSB_ERROR_NO_DEVICE, wenn keiner steckt. *claimed sagt, ob der
 * Interface-Claim geklappt hat (Reads koennen es teils auch ohne versuchen). */
int rtl_open_first(libusb_context *ctx, libusb_device_handle **out_handle,
                   uint16_t *out_pid, int *claimed);

/* Register-Zugriff. len ist 1, 2 oder 4. Rueckgabe: 0 ok, sonst libusb-Fehler. */
int rtl_reg_read(libusb_device_handle *h, uint16_t addr, uint8_t *buf, uint16_t len);
int rtl_reg_write(libusb_device_handle *h, uint16_t addr, const uint8_t *buf, uint16_t len);

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

#endif /* RTL_USB_H */
