/*
 * rtl_cal — RF-Kalibrierung (IQK + LCK) fuer den RTL8812AU (macOS, libusb).
 *
 * Portiert aus dem Linux-Treiber (aircrack-ng/rtl8812au), phydm/halrf:
 *   hal/phydm/halrf/rtl8812a/halrf_8812a_ce.c
 *     phy_iq_calibrate_8812a() / _phy_iq_calibrate_8812a() / _iqk_tx_8812a()
 *     phy_lc_calibrate_8812a() / _phy_lc_calibrate_8812a()
 *
 * Laeuft NACH RF-Init, VOR set_channel (siehe INTEGRATION-Kommentar in rtl_cal.c).
 */
#ifndef RTL_CAL_H
#define RTL_CAL_H

#include <libusb.h>

/*
 * Board-/RFE-abhaengige Parameter. Werden aus efuse/Board-Config abgeleitet.
 * Fuer die Alfa AWUS036ACH (RTL8812AU ohne externen PA/LNA) sind die Defaults
 * (alle 0) korrekt. Falls das efuse eine RFE-Type / External-PA meldet, kann
 * der Integrator diese Globals VOR rtl_iqk() setzen.
 *   rtl_cal_rfe_type   : RFE-Type (Default 0)
 *   rtl_cal_ext_pa_2g  : externer PA im 2.4-GHz-Band (0/1)
 *   rtl_cal_ext_pa_5g  : externer PA im 5-GHz-Band   (0/1)
 */
extern int rtl_cal_rfe_type;
extern int rtl_cal_ext_pa_2g;
extern int rtl_cal_ext_pa_5g;

/*
 * IQ-Kalibrierung (LOK + TX-IQK + RX-IQK, Pfad A und B).
 * Sichert MAC/BB/AFE/RF-Register, kalibriert, schreibt die IQC-Ergebnisse in
 * die BB-Register und stellt die gesicherten Register wieder her.
 * Das aktuelle Band (2.4/5 GHz) wird aus RF-Register 0x18 gelesen.
 * verbose!=0 druckt Fortschritt.
 * Rueckgabe: 0 ok, <0 wenn USB-Registerzugriffe fehlgeschlagen sind.
 */
int rtl_iqk(libusb_device_handle *h, int verbose);

/*
 * LC-Kalibrierung (VCO/LC-Tank). verbose!=0 druckt Fortschritt.
 * Rueckgabe: 0 ok, <0 bei USB-Fehler.
 */
int rtl_lck(libusb_device_handle *h, int verbose);

#endif /* RTL_CAL_H */
