/*
 * rtl_rx — Empfangspfad (Monitor) fuer RTL8812AU (macOS, libusb).
 *
 * Liest rohe 802.11-Frames vom Bulk-IN-Endpoint 0x81, deaggregiert die
 * USB-RX-Subframes anhand des 24-Byte-RX-Deskriptors und schreibt sie als
 * pcap mit Radiotap-Header (DLT 127) — direkt in Wireshark/tshark ladbar.
 *
 * RX-Deskriptor (aus include/rtl8812a_recv.h):
 *   dword0 bits0-13 = pkt_len, bit14 = crc_err, bit15 = icv_err,
 *          bits16-19 = drvinfo_size (x8 Byte), bits24-25 = shift
 *   dword2 bit28 = rpt_sel (1 = C2H/Report, kein normaler Frame)
 * Frame beginnt bei RXDESC_SIZE(24) + drvinfo_sz + shift_sz, Laenge pkt_len.
 */
#ifndef RTL_RX_H
#define RTL_RX_H

#include <stdio.h>
#include <libusb.h>

#define RTL_RX_EP        0x81
#define RTL_RXDESC_SIZE  24

/* pcap-Global-Header (Radiotap) in die Datei schreiben. */
void rtl_rx_write_pcap_header(FILE *f);

/* Ab jetzt fuer 'seconds' Sekunden Frames von 0x81 lesen und als pcap in f
 * schreiben. Gibt die Anzahl geschriebener Frames zurueck. verbose!=0 druckt
 * eine Live-Zaehlung. Setzt voraus: Chip initialisiert + Monitor-RCR + Kanal. */
long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose);

/* Wie rtl_rx_capture, aber zeitgeboxt in Millisekunden — fuer verschachteltes
 * TX/RX (injizieren, dann kurz lauschen). Gibt geschriebene Frames zurueck. */
long rtl_rx_pump(libusb_device_handle *h, FILE *f, int ms);

/* Endlos-Capture (fuer Wireshark-extcap): schreibt Frames nach f, bis ein
 * Schreibfehler auftritt (FIFO von Wireshark geschlossen). SIGPIPE vorher
 * ignorieren. Gibt geschriebene Frames zurueck. */
long rtl_rx_stream(libusb_device_handle *h, FILE *f);

#endif /* RTL_RX_H */
