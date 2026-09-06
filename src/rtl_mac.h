/*
 * rtl_mac — MAC-Initialisierung fuer den RTL8812AU (macOS, libusb, userspace).
 *
 * Portiert aus dem Linux-Treiber (aircrack-ng/rtl8812au, GPLv2):
 *   - hal/rtl8812a/rtl8812a_phycfg.c : PHY_MACConfig8812
 *   - hal/phydm/rtl8812a/halhwimg8812a_mac.c : array_mp_8812a_mac_reg[]
 *   - hal/rtl8812a/usb/usb_halinit.c : die _Init*_8812AUsb MAC-Bring-up-Schritte
 *   - hal/rtl8812a/rtl8812a_hal_init.c : InitLLTTable8812A, _InitBeaconParameters,
 *                                        hw_var_set_monitor (Monitor-RCR)
 *
 * Voraussetzung: Chip ist bereits eingeschaltet (rtl_power_on) und die Firmware
 * ist geladen (rtl_fw_download). Register-Zugriff via rtl_usb.h.
 */
#ifndef RTL_MAC_H
#define RTL_MAC_H

#include <stdint.h>
#include <libusb.h>

/* Vollstaendiger MAC-Bring-up (LLT, MAC-Reg-Tabelle, Queues/Pages, WMAC, EDCA,
 * Retry, Beacon, Burst-Pkt-Len, dann CR|MACTXEN|MACRXEN).
 * verbose!=0 druckt jeden Schritt.
 * Rueckgabe: 0 ok, <0 USB-Fehler, >0 = LLT-Polling-Timeout (Schrittnummer). */
int rtl_mac_init(libusb_device_handle *h, int verbose);

/* Konfiguriert den MAC fuer MONITOR MODE: promiskuitives REG_RCR (alle
 * Frame-Typen inkl. CRC-/ICV-Fehler) und RXFLTMAP0/1/2 = 0xFFFF (alle
 * mgmt/ctrl/data-Subtypen). Setzt zudem den Netzwerktyp auf NoLink.
 * Sollte NACH rtl_mac_init aufgerufen werden. verbose!=0 druckt Details.
 * Rueckgabe: 0 ok, <0 USB-Fehler. */
int rtl_mac_set_monitor(libusb_device_handle *h, int verbose);

#endif /* RTL_MAC_H */
