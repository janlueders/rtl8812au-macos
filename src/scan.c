/*
 * scan — WLAN scanner (first milestone toward an internet client).
 *
 * Brings the chip up, hops across 2.4- and 5-GHz channels, collects beacons
 * and probe responses, and lists the networks found (SSID, BSSID, channel,
 * encryption). Receive only, no TX.
 *
 * Usage: ./scan [ms_per_channel]   (default 500 ms)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rf.h"
#include "rtl_rx.h"

typedef struct { uint8_t bssid[6]; char ssid[33]; int channel; int privacy; } net_t;
typedef struct { net_t nets[512]; int count; int cur_ch; } scan_ctx;

static void scan_cb(const uint8_t *f, uint32_t len, void *v) {
    scan_ctx *s = (scan_ctx *)v;
    if (len < 38) return;
    uint8_t fc = f[0];
    if (fc != 0x80 && fc != 0x50) return;          /* Beacon (0x80) / Probe-Resp (0x50) */
    const uint8_t *bssid = f + 16;                  /* addr3 */
    uint16_t caps = (uint16_t)(f[34] | (f[35] << 8)); /* Capability (24+8+2) */
    int privacy = (caps & 0x0010) ? 1 : 0;

    char ssid[33] = "";
    const uint8_t *ie = f + 36; int rem = (int)len - 36;
    while (rem >= 2) {
        int id = ie[0], l = ie[1];
        if (2 + l > rem) break;
        if (id == 0) { int n = l > 32 ? 32 : l; memcpy(ssid, ie + 2, n); ssid[n] = 0; break; }
        ie += 2 + l; rem -= 2 + l;
    }

    for (int i = 0; i < s->count; i++)
        if (memcmp(s->nets[i].bssid, bssid, 6) == 0) return;   /* already known */
    if (s->count < 512) {
        net_t *n = &s->nets[s->count++];
        memcpy(n->bssid, bssid, 6);
        strncpy(n->ssid, ssid[0] ? ssid : "<versteckt>", 32);
        n->channel = s->cur_ch; n->privacy = privacy;
    }
}

int main(int argc, char **argv) {
    int dwell = (argc > 1) ? atoi(argv[1]) : 500;
    static const int chans[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,
        36,40,44,48,52,56,60,64,100,104,108,112,116,132,136,140,149,153,157,161,165};

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc || !h) { printf("Kein RTL8812AU gefunden.\n"); libusb_exit(ctx); return 2; }
    printf("Geraet 0bda:%04x. Inbetriebnahme ...\n", pid);
    if (rtl_hal_full_init(h, 1, 0, 0) != 0) { printf("Init fehlgeschlagen.\n"); goto done; }

    scan_ctx s; memset(&s, 0, sizeof(s));
    printf("Scanne %zu Kanaele (%d ms/Kanal) ...\n", sizeof(chans)/sizeof(int), dwell);
    for (size_t i = 0; i < sizeof(chans)/sizeof(int); i++) {
        s.cur_ch = chans[i];
        if (rtl_rf_set_channel(h, chans[i], 0) != 0) continue;
        rtl_rx_poll(h, dwell, scan_cb, &s);
        printf("\r  Kanal %3d  gefunden: %d Netze ", chans[i], s.count); fflush(stdout);
    }
    printf("\n\n%-32s %-18s %-5s %s\n", "SSID", "BSSID", "Kanal", "Verschl.");
    printf("--------------------------------------------------------------------------\n");
    for (int i = 0; i < s.count; i++) {
        net_t *n = &s.nets[i];
        printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x %-5d %s\n",
               n->ssid, n->bssid[0],n->bssid[1],n->bssid[2],n->bssid[3],n->bssid[4],n->bssid[5],
               n->channel, n->privacy ? "WPA/WEP" : "offen");
    }
    printf("\n%d Netze gefunden.\n", s.count);

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
