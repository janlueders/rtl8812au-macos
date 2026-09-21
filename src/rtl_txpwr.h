/*
 * rtl_txpwr — apply the chip's own efuse-calibrated TX power (RTL8812AU).
 *
 * Minimal port of the reference driver's PHY_GetTxPowerIndexBase /
 * PHY_SetTxPowerIndex_8812A (hal/hal_com_phycfg.c, hal/rtl8812a/rtl8812a_phycfg.c),
 * scoped to what this driver actually transmits: legacy CCK (1M) and OFDM
 * (6M/54M) rates on RF path A, 2.4GHz, 20MHz bandwidth. No HT/VHT, no
 * dynamic thermal tracking -- just the one-time base calibration every real
 * driver applies at init.
 */
#ifndef RTL_TXPWR_H
#define RTL_TXPWR_H

#include "rtl_usb.h"

/* Read the efuse PG (power-group) table and program the BB TX-AGC index
 * registers for 'channel' (2.4GHz, 1-14) with the chip's own calibrated
 * values -- the same data and registers the stock vendor driver uses, so
 * this cannot exceed the chip's rated/legal maximum. If the efuse PG data
 * reads as unprogrammed/invalid, this leaves the chip's current power
 * setting untouched (never guesses or writes an arbitrary value) and says
 * so when verbose. Returns 0 on success, otherwise a libusb error from the
 * efuse read. */
int rtl_txpwr_apply(libusb_device_handle *h, int channel, int verbose);

/* Read back the current TX-AGC index registers and print them (always, not
 * gated on verbose) -- used right after rtl_txpwr_apply() and again at the
 * end of rtl_hal_full_init() to catch anything downstream (calibration,
 * monitor-RCR setup) that resets these registers without us noticing. */
void rtl_txpwr_readback(libusb_device_handle *h, const char *label);

/* Periodic thermal compensation (ported from the reference driver's real,
 * actively-used odm_txpowertracking_callback_thermal_meter + the RFE-3
 * delta-swing tables -- verified applicable to this unit via efuseinfo,
 * not invented). Call this every few seconds during a live connection,
 * after rtl_txpwr_apply() has run at least once. Re-reads the efuse thermal
 * baseline (0xBA) and RFE type (0xCA) once and caches them; if the baseline
 * is unprogrammed or the RFE type isn't 3 (the only table ported here), this
 * is a safe no-op, matching how the real driver also skips tracking rather
 * than guessing. Only adjusts the OFDM TX-AGC registers (what this driver
 * actually transmits with); the CCK register is left at its efuse base. */
void rtl_txpwr_thermal_track(libusb_device_handle *h);

#endif /* RTL_TXPWR_H */
