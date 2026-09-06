/*
 * rtl_cal — RF calibration (IQK + LCK) for the RTL8812AU (macOS, libusb).
 *
 * Ported from the Linux driver (aircrack-ng/rtl8812au), phydm/halrf:
 *   hal/phydm/halrf/rtl8812a/halrf_8812a_ce.c
 *     phy_iq_calibrate_8812a() / _phy_iq_calibrate_8812a() / _iqk_tx_8812a()
 *     phy_lc_calibrate_8812a() / _phy_lc_calibrate_8812a()
 *
 * Runs AFTER RF-Init, BEFORE set_channel (see INTEGRATION comment in rtl_cal.c).
 */
#ifndef RTL_CAL_H
#define RTL_CAL_H

#include <libusb.h>

/*
 * Board/RFE-dependent parameters. Derived from efuse/board config.
 * For the Alfa AWUS036ACH (RTL8812AU without external PA/LNA) the defaults
 * (all 0) are correct. If the efuse reports an RFE type / external PA, the
 * integrator can set these globals BEFORE rtl_iqk().
 *   rtl_cal_rfe_type   : RFE type (default 0)
 *   rtl_cal_ext_pa_2g  : external PA in the 2.4-GHz band (0/1)
 *   rtl_cal_ext_pa_5g  : external PA in the 5-GHz band   (0/1)
 */
extern int rtl_cal_rfe_type;
extern int rtl_cal_ext_pa_2g;
extern int rtl_cal_ext_pa_5g;

/*
 * IQ calibration (LOK + TX-IQK + RX-IQK, path A and B).
 * Backs up MAC/BB/AFE/RF registers, calibrates, writes the IQC results into
 * the BB registers and restores the backed-up registers.
 * The current band (2.4/5 GHz) is read from RF register 0x18.
 * verbose!=0 prints progress.
 * Return: 0 ok, <0 if USB register accesses failed.
 */
int rtl_iqk(libusb_device_handle *h, int verbose);

/*
 * LC calibration (VCO/LC tank). verbose!=0 prints progress.
 * Return: 0 ok, <0 on USB error.
 */
int rtl_lck(libusb_device_handle *h, int verbose);

#endif /* RTL_CAL_H */
