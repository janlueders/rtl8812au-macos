
#ifndef RTL_RX_H
#define RTL_RX_H

#include <stdio.h>
#include <libusb.h>
#include "rtl_chip.h"

#define RTL_RX_EP        0x81
#define RTL_RXDESC_SIZE  24

/* Anzahl gleichzeitig beim Host-Controller liegender URBs. */
#define RTL_RX_URBS      8

/*
 * MUSS einmal nach libusb_init()/rtl_open_first() aufgerufen werden, um den
 * asynchronen RX-Pfad zu aktivieren. ops darf NULL sein (-> rtl_chip_default).
 * Ohne diesen Aufruf faellt rtl_rx transparent auf den alten synchronen Pfad
 * zurueck -- nichts bricht, es ist nur langsamer.
 */
void rtl_rx_bind(libusb_context *ctx, const rtl_chip_ops *ops);

/* MUSS vor libusb_release_interface()/libusb_close() aufgerufen werden:
 * stoppt die persistente Engine und wartet, bis alle URBs zurueck sind.
 * Ohne das laufen beim Schliessen noch Transfers auf das Handle. */
void rtl_rx_unbind(void);

void rtl_rx_write_pcap_header(FILE *f);

long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose);
long rtl_rx_pump   (libusb_device_handle *h, FILE *f, int ms);
long rtl_rx_stream (libusb_device_handle *h, FILE *f);

typedef void (*rtl_frame_cb)(const uint8_t *frame, uint32_t len, void *ctx);
long rtl_rx_poll(libusb_device_handle *h, int ms, rtl_frame_cb cb, void *ctx);

#endif /* RTL_RX_H */