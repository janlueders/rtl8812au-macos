#include "rtl_rx.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#define RX_BUF_SIZE  32768
#define RND8(x)      (((x) + 7) & ~7u)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void rtl_rx_write_pcap_header(FILE *f) {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t vmaj = 2, vmin = 4;
    uint32_t zone = 0, sig = 0, snap = 65535, net = 127; /* 127 = LINKTYPE_IEEE802_11_RADIOTAP */
    fwrite(&magic, 4, 1, f);
    fwrite(&vmaj, 2, 1, f); fwrite(&vmin, 2, 1, f);
    fwrite(&zone, 4, 1, f); fwrite(&sig, 4, 1, f);
    fwrite(&snap, 4, 1, f); fwrite(&net, 4, 1, f);
    fflush(f);
}

/* Minimaler Radiotap-Header (8 Byte, keine Felder) + 802.11-Frame als pcap-Record. */
static void write_frame(FILE *f, const uint8_t *frame, uint32_t len) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint8_t rtap[8] = { 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 }; /* ver,pad,len=8,present=0 */
    uint32_t caplen = len + sizeof(rtap);
    uint32_t sec = (uint32_t)ts.tv_sec, usec = (uint32_t)(ts.tv_nsec / 1000);
    fwrite(&sec, 4, 1, f); fwrite(&usec, 4, 1, f);
    fwrite(&caplen, 4, 1, f); fwrite(&caplen, 4, 1, f);
    fwrite(rtap, sizeof(rtap), 1, f);
    fwrite(frame, 1, len, f);
}

/* Einen USB-RX-Transfer (ggf. mehrere aggregierte Subframes) verarbeiten. */
static long parse_bulk(const uint8_t *buf, int total, FILE *f) {
    long n = 0;
    const uint8_t *pbuf = buf;
    int transfer_len = total;

    while (transfer_len > RTL_RXDESC_SIZE) {
        uint32_t d0 = le32(pbuf);
        uint32_t d2 = le32(pbuf + 8);
        uint32_t pkt_len    = d0 & 0x3FFF;
        uint32_t drvinfo_sz = ((d0 >> 16) & 0x0F) * 8;
        uint32_t shift_sz   = (d0 >> 24) & 0x03;
        uint32_t rpt_sel    = (d2 >> 28) & 0x01;

        uint32_t pkt_offset = RTL_RXDESC_SIZE + drvinfo_sz + shift_sz + pkt_len;
        if (pkt_len == 0 || (int)pkt_offset > transfer_len) break;

        if (!rpt_sel) { /* normaler 802.11-Frame (auch crc_err: Monitor will alles) */
            const uint8_t *frame = pbuf + RTL_RXDESC_SIZE + drvinfo_sz + shift_sz;
            write_frame(f, frame, pkt_len);
            n++;
        }
        uint32_t adv = RND8(pkt_offset);
        if ((int)adv > transfer_len) break;
        pbuf += adv;
        transfer_len -= adv;
    }
    return n;
}

long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) return -1;

    long total_frames = 0;
    time_t end = time(NULL) + seconds;

    while (time(NULL) < end) {
        int got = 0;
        int rc = libusb_bulk_transfer(h, RTL_RX_EP, buf, RX_BUF_SIZE, &got, 300);
        if (rc == 0 && got > RTL_RXDESC_SIZE) {
            long n = parse_bulk(buf, got, f);
            total_frames += n;
            if (verbose && n > 0) { printf("\r  Frames: %ld ", total_frames); fflush(stdout); }
        } else if (rc == LIBUSB_ERROR_TIMEOUT) {
            continue; /* nichts empfangen, weiter */
        } else if (rc != 0) {
            if (verbose) printf("\n  bulk_transfer rc=%d (%s)\n", rc, libusb_error_name(rc));
            break;
        }
    }
    fflush(f);
    free(buf);
    if (verbose) printf("\n");
    return total_frames;
}
