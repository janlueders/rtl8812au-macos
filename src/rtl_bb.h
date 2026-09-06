/*
 * rtl_bb — Baseband/PHY init for the RTL8812AU (macOS, libusb userspace).
 *
 * Ported from the aircrack-ng/rtl8812au Linux driver (GPLv2):
 *   hal/rtl8812a/rtl8812a_phycfg.c        (PHY_BBConfig8812, phy_BB8812_Config_ParaFile)
 *   hal/phydm/rtl8812a/halhwimg8812a_bb.c (array_mp_8812a_phy_reg[], array_mp_8812a_agc_tab[],
 *                                          odm_read_and_config_* apply loops)
 *   hal/phydm/rtl8812a/phydm_regconfig8812a.c (odm_config_bb_phy_8812a / _agc_8812a delays)
 */
#ifndef RTL_BB_H
#define RTL_BB_H

#include "rtl_usb.h"

/*
 * Baseband + PHY init: BB reset/enable sequence, then the PHY_REG table,
 * then the AGC table. Must run AFTER MAC init and BEFORE RF init.
 * verbose != 0 prints each phase / register write.
 * Returns 0 on success, <0 on the first USB error.
 */
int rtl_bb_init(libusb_device_handle *h, int verbose);

/*
 * Baseband register helpers (phy_query_bb_reg / phy_set_bb_reg semantics).
 * BB registers live in 0x800..0xFFF and are plain 32-bit MMIO via rtl_read32/rtl_write32.
 * The shift is derived from the lowest set bit of bitmask.
 */
uint32_t bb_get(libusb_device_handle *h, uint16_t addr, uint32_t bitmask);
int      bb_set(libusb_device_handle *h, uint16_t addr, uint32_t bitmask, uint32_t data);

#endif /* RTL_BB_H */
