#include "rtl_rx.h"
#include "rtl_usb.h"     /* rtl_led_on/off fuer Aktivitaets-Blinken */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

/* LED bei Funkverkehr blinken lassen, gedrosselt auf ~120ms Umschaltung. */
static void led_activity(libusb_device_handle *h, int active) {
    static struct timespec last; static int inited = 0, state = 0;
    if (!active) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!inited) { last = now; inited = 1; }
    long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (ms >= 120) {
        state = !state;
        if (state) rtl_led_on(h); else rtl_led_off(h);
        last = now;
    }
}

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

long rtl_rx_stream(libusb_device_handle *h, FILE *f) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) return -1;
    long frames = 0;
    while (1) {
        int got = 0;
        int rc = libusb_bulk_transfer(h, RTL_RX_EP, buf, RX_BUF_SIZE, &got, 300);
        if (rc == 0 && got > RTL_RXDESC_SIZE) {
            frames += parse_bulk(buf, got, f);
            led_activity(h, 1);
            fflush(f);
            if (ferror(f)) break;   /* FIFO geschlossen -> Ende */
        } else if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            break;                  /* Geraet weg */
        }
    }
    free(buf);
    return frames;
}

long rtl_rx_poll(libusb_device_handle *h, int ms, rtl_frame_cb cb, void *ctx) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) return -1;
    long frames = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long el = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) break;
        int got = 0;
        int rc = libusb_bulk_transfer(h, RTL_RX_EP, buf, RX_BUF_SIZE, &got, 50);
        if (rc != 0 || got <= RTL_RXDESC_SIZE) continue;
        const uint8_t *pbuf = buf; int tl = got;
        while (tl > RTL_RXDESC_SIZE) {
            uint32_t d0 = le32(pbuf), d2 = le32(pbuf + 8);
            uint32_t pkt_len = d0 & 0x3FFF, drv = ((d0 >> 16) & 0xF) * 8, sh = (d0 >> 24) & 3;
            uint32_t rpt = (d2 >> 28) & 1;
            uint32_t off = RTL_RXDESC_SIZE + drv + sh + pkt_len;
            if (pkt_len == 0 || (int)off > tl) break;
            if (!rpt) { cb(pbuf + RTL_RXDESC_SIZE + drv + sh, pkt_len, ctx); frames++; }
            uint32_t adv = RND8(off);
            if ((int)adv > tl) break;
            pbuf += adv; tl -= adv;
        }
    }
    free(buf);
    return frames;
}

long rtl_rx_pump(libusb_device_handle *h, FILE *f, int ms) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) return -1;
    long frames = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long el = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) break;
        int got = 0;
        int rc = libusb_bulk_transfer(h, RTL_RX_EP, buf, RX_BUF_SIZE, &got, 50);
        if (rc == 0 && got > RTL_RXDESC_SIZE)
            frames += parse_bulk(buf, got, f);
    }
    free(buf);
    return frames;
}

long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) return -1;

    long total_frames = 0;
    long xfers = 0;            /* nicht-leere Bulk-Transfers */
    long total_bytes = 0;      /* rohe empfangene Bytes */
    long timeouts = 0;
    uint32_t first_d0 = 0; int have_first = 0;
    time_t end = time(NULL) + seconds;

    while (time(NULL) < end) {
        int got = 0;
        int rc = libusb_bulk_transfer(h, RTL_RX_EP, buf, RX_BUF_SIZE, &got, 300);
        if (rc == 0 && got > 0) {
            xfers++; total_bytes += got;
            led_activity(h, 1);
            if (!have_first) { first_d0 = le32(buf); have_first = 1; }
            if (got > RTL_RXDESC_SIZE) {
                long n = parse_bulk(buf, got, f);
                total_frames += n;
            }
            if (verbose) { printf("\r  Transfers:%ld Bytes:%ld Frames:%ld ", xfers, total_bytes, total_frames); fflush(stdout); }
        } else if (rc == LIBUSB_ERROR_TIMEOUT) {
            timeouts++;
            continue;
        } else if (rc != 0) {
            if (verbose) printf("\n  bulk_transfer rc=%d (%s)\n", rc, libusb_error_name(rc));
            break;
        }
    }
    fflush(f);
    free(buf);
    if (verbose) {
        printf("\n  DIAGNOSE: Transfers=%ld  Bytes=%ld  Frames=%ld  Timeouts=%ld\n",
               xfers, total_bytes, total_frames, timeouts);
        if (have_first) printf("  Erster RX-Deskriptor dword0 = 0x%08x (pkt_len=%u)\n",
                               first_d0, first_d0 & 0x3FFF);
        else printf("  KEINE rohen Bytes vom Chip empfangen (0x81 liefert nichts).\n");
    }
    return total_frames;
}
