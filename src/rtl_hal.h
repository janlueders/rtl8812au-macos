/*
 * rtl_hal — Orchestrator: vollstaendiges Bring-up ueber rtl_chip_ops.
 *
 * Die Reihenfolge (Power-On -> Firmware -> MAC -> BB -> RF -> Kanal ->
 * Kalibrierung -> Monitor-RCR) ist chip-unabhaengig; WAS jeder Schritt tut,
 * liefert die ops-Tabelle aus rtl_chip.h.
 */
#ifndef RTL_HAL_H
#define RTL_HAL_H

#include <stdint.h>
#include <libusb.h>
#include "rtl_chip.h"

typedef struct {
    libusb_device_handle *usb;
    const rtl_chip_ops   *ops;
    uint16_t              pid;
    int                   channel;   /* zuletzt erfolgreich gesetzter Kanal */
} rtl_dev;

/* Bindet die ops-Tabelle anhand der USB-PID. Danach ist dev nutzbar.
 * Unbekannte PID -> rtl_chip_default() (Verhalten wie bisher). */
void rtl_dev_bind(rtl_dev *dev, libusb_device_handle *h, uint16_t pid);

/* Wie rtl_dev_bind, holt die PID aber selbst aus dem USB-Descriptor. */
void rtl_dev_bind_auto(rtl_dev *dev, libusb_device_handle *h);

/* Bring-up ueber die ops-Tabelle. with_cal=1 fuegt LCK+IQK hinzu.
 * Return: 0 ok, sonst der rc des fehlgeschlagenen Schritts. */
int rtl_hal_init_dev(rtl_dev *dev, int channel, int with_cal, int verbose);

/* Kanalwechsel nach dem Bring-up (aktualisiert dev->channel und zieht die
 * TX-Power fuer den neuen Kanal nach -- das fehlte bisher beim Scannen). */
int rtl_hal_set_channel(rtl_dev *dev, int channel, int verbose);

/* Kompatibilitaets-Wrapper: bindet die ops selbst. Bestehende Tools
 * brauchen keine Aenderung. */
int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose);

#endif /* RTL_HAL_H */