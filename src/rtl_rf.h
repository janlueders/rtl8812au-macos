/*
 * rtl_rf — RF-Initialisierung und Kanalsteuerung fuer den RTL8812AU (macOS, libusb).
 *
 * Portiert (userspace) aus dem Linux-Treiber aircrack-ng/rtl8812au:
 *   hal/rtl8812a/rtl8812a_phycfg.c   (phy_RFSerialRead/Write, PHY_SwChnl8812, ...)
 *   hal/rtl8812a/rtl8812a_rf6052.c   (PHY_RF6052_Config / SetBandwidth)
 *   hal/phydm/rtl8812a/halhwimg8812a_rf.c        (radioA/radioB Register-Tabellen)
 *   hal/phydm/rtl8812a/phydm_regconfig8812a.c    (RF-Tabellen-Apply-Loop)
 *
 * RF-Register werden NICHT direkt per USB gelesen/geschrieben, sondern indirekt
 * ueber die Baseband-LSSI-Register (3-wire-Interface). Dieses Modul ist
 * self-contained und benoetigt nur rtl_read32/rtl_write32 aus rtl_usb.h.
 */
#ifndef RTL_RF_H
#define RTL_RF_H

#include <stdint.h>
#include <libusb.h>

/* RF-Pfade */
#define RTL_RF_PATH_A   0
#define RTL_RF_PATH_B   1

/* Bandbreiten fuer rtl_rf_set_channel() */
#define RTL_BW_20   0   /* 20 MHz  (implementiert) */
#define RTL_BW_40   1   /* 40 MHz  (Platzhalter, nicht vollstaendig) */
#define RTL_BW_80   2   /* 80 MHz  (Platzhalter, nicht vollstaendig) */

/*
 * Ein einzelnes RF-Register lesen (20-bit-Wert, Bits [19:0]).
 * path: 0 = RF_PATH_A, 1 = RF_PATH_B.
 * *rc (darf NULL sein) erhaelt 0 bei Erfolg bzw. einen libusb-Fehlercode (<0).
 */
uint32_t rtl_rf_read(libusb_device_handle *h, int path, uint16_t reg_addr, int *rc);

/*
 * Ein einzelnes RF-Register schreiben (nur die unteren 20 Bit von data zaehlen).
 * Rueckgabe: 0 ok, sonst libusb-Fehler (<0).
 */
int rtl_rf_write(libusb_device_handle *h, int path, uint16_t reg_addr, uint32_t data);

/*
 * RF-Grundkonfiguration: wendet die radioA-Tabelle auf Pfad A und die
 * radioB-Tabelle auf Pfad B an (entspricht PHY_RFConfig8812).
 * Voraussetzung: Power-On, Firmware, MAC- und BB-Init sind bereits erfolgt.
 * verbose!=0 druckt Fortschritt.
 * Rueckgabe: 0 ok, sonst libusb-Fehler (<0).
 */
int rtl_rf_init(libusb_device_handle *h, int verbose);

/*
 * Kanal (und Bandbreite) einstellen.
 *   ch : 1..14 (2.4 GHz) oder 36..165 (5 GHz)
 *   bw : RTL_BW_20 (0). 40/80 sind Platzhalter.
 * Fuehrt Band-Umschaltung (2.4/5 GHz), Kanal-Register (RF 0x18) und die
 * 20-MHz-Bandbreiten-Register (BB 0x8ac u.a. + RF 0x18[11:10]) aus.
 * Rueckgabe: 0 ok, <0 libusb-Fehler, >0 = ungueltiges Argument.
 */
int rtl_rf_set_channel(libusb_device_handle *h, int ch, int bw);

#endif /* RTL_RF_H */
