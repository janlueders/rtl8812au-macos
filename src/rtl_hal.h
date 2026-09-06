/*
 * rtl_hal — Orchestrator: volle Inbetriebnahme des RTL8812AU bis Monitor Mode.
 *
 * Faedelt die Einzelmodule in der richtigen Reihenfolge zusammen:
 *   Power-On -> Firmware -> MAC-Init -> BB-Init -> RF-Init -> LCK -> IQK
 *   -> Kanal setzen -> Monitor-RCR -> LED an.
 */
#ifndef RTL_HAL_H
#define RTL_HAL_H

#include <libusb.h>

/* Volle Inbetriebnahme inkl. Monitor-Konfiguration auf 'channel' (20 MHz).
 * with_cal=0: reiner RX-Hochlauf ohne internes TX (kein PA-Risiko).
 * with_cal=1: zusaetzlich LCK+IQK (fuer Injection-Qualitaet).
 * Rueckgabe 0 ok, sonst != 0 (der fehlgeschlagene Schritt wird gedruckt). */
int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose);

#endif /* RTL_HAL_H */
