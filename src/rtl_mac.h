/*
 * rtl_mac — MAC initialization for the RTL8812AU (macOS, libusb, userspace).
 *
 * Ported from the Linux driver (aircrack-ng/rtl8812au, GPLv2):
 *   - hal/rtl8812a/rtl8812a_phycfg.c : PHY_MACConfig8812
 *   - hal/phydm/rtl8812a/halhwimg8812a_mac.c : array_mp_8812a_mac_reg[]
 *   - hal/rtl8812a/usb/usb_halinit.c : the _Init*_8812AUsb MAC bring-up steps
 *   - hal/rtl8812a/rtl8812a_hal_init.c : InitLLTTable8812A, _InitBeaconParameters,
 *                                        hw_var_set_monitor (Monitor-RCR)
 *
 * Precondition: chip is already powered on (rtl_power_on) and the firmware
 * is loaded (rtl_fw_download). Register access via rtl_usb.h.
 */
#ifndef RTL_MAC_H
#define RTL_MAC_H

#include <stdint.h>
#include <libusb.h>

/* Full MAC bring-up (LLT, MAC-reg table, queues/pages, WMAC, EDCA,
 * Retry, Beacon, Burst pkt len, then CR|MACTXEN|MACRXEN).
 * verbose!=0 prints every step.
 * Return: 0 ok, <0 USB error, >0 = LLT polling timeout (step number). */
int rtl_mac_init(libusb_device_handle *h, int verbose);

/* Configures the MAC for MONITOR MODE: promiscuous REG_RCR (all
 * frame types incl. CRC/ICV errors) and RXFLTMAP0/1/2 = 0xFFFF (all
 * mgmt/ctrl/data subtypes). Also sets the network type to NoLink.
 * Should be called AFTER rtl_mac_init. verbose!=0 prints details.
 * Return: 0 ok, <0 USB error. */
int rtl_mac_set_monitor(libusb_device_handle *h, int verbose);

#endif /* RTL_MAC_H */
