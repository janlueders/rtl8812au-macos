/*
 * rtl_rx — receive path (monitor) for RTL8812AU (macOS, libusb).
 *
 * Reads raw 802.11 frames from the bulk-IN endpoint 0x81, de-aggregates the
 * USB-RX subframes using the 24-byte RX descriptor and writes them as
 * pcap with a radiotap header (DLT 127) — directly loadable in Wireshark/tshark.
 *
 * RX descriptor (from include/rtl8812a_recv.h):
 *   dword0 bits0-13 = pkt_len, bit14 = crc_err, bit15 = icv_err,
 *          bits16-19 = drvinfo_size (x8 bytes), bits24-25 = shift
 *   dword2 bit28 = rpt_sel (1 = C2H/report, not a normal frame)
 * Frame starts at RXDESC_SIZE(24) + drvinfo_sz + shift_sz, length pkt_len.
 */
#ifndef RTL_RX_H
#define RTL_RX_H

#include <stdio.h>
#include <libusb.h>

#define RTL_RX_EP        0x81
#define RTL_RXDESC_SIZE  24

/* Write the pcap global header (radiotap) to the file. */
void rtl_rx_write_pcap_header(FILE *f);

/* From now, for 'seconds' seconds, read frames from 0x81 and write them as pcap
 * into f. Returns the number of frames written. verbose!=0 prints a live count.
 * Requires: chip initialized + monitor RCR + channel. */
long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose);

/* Like rtl_rx_capture, but time-boxed in milliseconds — for interleaved
 * TX/RX (inject, then listen briefly). Returns the frames written. */
long rtl_rx_pump(libusb_device_handle *h, FILE *f, int ms);

/* Endless capture (for Wireshark extcap): writes frames to f until a write
 * error occurs (Wireshark's FIFO closed). Ignore SIGPIPE beforehand.
 * Returns the frames written. */
long rtl_rx_stream(libusb_device_handle *h, FILE *f);

/* Callback-based polling: read frames for 'ms' milliseconds and call
 * cb(frame, len, ctx) for each decoded 802.11 frame. For scan/analysis. */
typedef void (*rtl_frame_cb)(const uint8_t *frame, uint32_t len, void *ctx);
long rtl_rx_poll(libusb_device_handle *h, int ms, rtl_frame_cb cb, void *ctx);

#endif /* RTL_RX_H */
