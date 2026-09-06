/*
 * rtl_rf — RF initialization and channel control for the RTL8812AU (macOS, libusb).
 *
 * Ported (userspace) from the Linux driver aircrack-ng/rtl8812au:
 *   hal/rtl8812a/rtl8812a_phycfg.c   (phy_RFSerialRead/Write, PHY_SwChnl8812, ...)
 *   hal/rtl8812a/rtl8812a_rf6052.c   (PHY_RF6052_Config / SetBandwidth)
 *   hal/phydm/rtl8812a/halhwimg8812a_rf.c        (radioA/radioB register tables)
 *   hal/phydm/rtl8812a/phydm_regconfig8812a.c    (RF table apply loop)
 *
 * RF registers are NOT read/written directly over USB, but indirectly
 * via the Baseband LSSI registers (3-wire interface). This module is
 * self-contained and only needs rtl_read32/rtl_write32 from rtl_usb.h.
 */
#ifndef RTL_RF_H
#define RTL_RF_H

#include <stdint.h>
#include <libusb.h>

/* RF paths */
#define RTL_RF_PATH_A   0
#define RTL_RF_PATH_B   1

/* Bandwidths for rtl_rf_set_channel() */
#define RTL_BW_20   0   /* 20 MHz  (implemented) */
#define RTL_BW_40   1   /* 40 MHz  (placeholder, not complete) */
#define RTL_BW_80   2   /* 80 MHz  (placeholder, not complete) */

/*
 * Read a single RF register (20-bit value, bits [19:0]).
 * path: 0 = RF_PATH_A, 1 = RF_PATH_B.
 * *rc (may be NULL) receives 0 on success or a libusb error code (<0).
 */
uint32_t rtl_rf_read(libusb_device_handle *h, int path, uint16_t reg_addr, int *rc);

/*
 * Write a single RF register (only the lower 20 bits of data count).
 * Return: 0 ok, otherwise libusb error (<0).
 */
int rtl_rf_write(libusb_device_handle *h, int path, uint16_t reg_addr, uint32_t data);

/*
 * Basic RF configuration: applies the radioA table to path A and the
 * radioB table to path B (equivalent to PHY_RFConfig8812).
 * Prerequisite: power-on, firmware, MAC and BB init have already been done.
 * verbose!=0 prints progress.
 * Return: 0 ok, otherwise libusb error (<0).
 */
int rtl_rf_init(libusb_device_handle *h, int verbose);

/*
 * Set channel (and bandwidth).
 *   ch : 1..14 (2.4 GHz) or 36..165 (5 GHz)
 *   bw : RTL_BW_20 (0). 40/80 are placeholders.
 * Performs band switching (2.4/5 GHz), the channel register (RF 0x18) and the
 * 20 MHz bandwidth registers (BB 0x8ac among others + RF 0x18[11:10]).
 * Return: 0 ok, <0 libusb error, >0 = invalid argument.
 */
int rtl_rf_set_channel(libusb_device_handle *h, int ch, int bw);

#endif /* RTL_RF_H */
