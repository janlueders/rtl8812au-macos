/*
 * inject — injection test (M5): sends broadcast probe requests and records
 * the responses. Probe responses prove that TX actually gets on the air.
 *
 * Usage: ./inject [channel] [count] [calibration 0/1] [output.pcap]
 * Default: channel 6, 30 probe requests, without calibration, inject.pcap
 *
 * SAFETY: moderate default TX power (index 0x12), management frames.
 * Only use on networks you are authorized for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"
#include "rtl_tx.h"

int main(int argc, char **argv) {
    int channel  = (argc > 1) ? atoi(argv[1]) : 6;
    int count    = (argc > 2) ? atoi(argv[2]) : 30;
    int with_cal = (argc > 3) ? atoi(argv[3]) : 0;
    const char *out = (argc > 4) ? argv[4] : "inject.pcap";

    /* Broadcast probe request, SA = our efuse MAC (Alfa OUI). */
    uint8_t sa[6] = { 0x00, 0xc0, 0xca, 0xbc, 0x4e, 0xfa };
    uint8_t probe[] = {
        0x40, 0x00,                         /* FC: Mgmt, Probe Request */
        0x00, 0x00,                         /* Duration */
        0xff,0xff,0xff,0xff,0xff,0xff,      /* DA: Broadcast */
        sa[0],sa[1],sa[2],sa[3],sa[4],sa[5],/* SA */
        0xff,0xff,0xff,0xff,0xff,0xff,      /* BSSID: Broadcast */
        0x00, 0x00,                         /* Seq (HW sets) */
        0x00, 0x00,                         /* SSID-Element, len 0 (Wildcard) */
        0x01, 0x08, 0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24, /* Supported Rates */
        0x32, 0x04, 0x30,0x48,0x60,0x6c     /* Extended Rates */
    };

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }
    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein RTL8812AU gefunden.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Geraet 0bda:%04x, Claim: %s\n\n", pid, claimed ? "OK" : "NEIN");

    rc = rtl_hal_full_init(h, channel, with_cal, 1);
    if (rc != 0) { printf("Init fehlgeschlagen (rc=%d).\n", rc); goto done; }

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); rc = 1; goto done; }
    rtl_rx_write_pcap_header(f);

    printf("\nInjiziere %d Broadcast-Probe-Requests auf Kanal %d (cal=%d)%s\n",
           count, channel, with_cal, with_cal ? "" : " [RX-only-Power, kein IQK]");

    long tx_ok = 0, tx_err = 0, resp = 0;
    for (int i = 0; i < count; i++) {
        int r = rtl_tx_inject(h, probe, (int)sizeof(probe), RTL_RATE_6M, RTL_QSLT_MGNT, RTL_TX_EP_MGMT);
        if (r == 0) tx_ok++; else { tx_err++; if (tx_err <= 3) printf("  TX-Fehler: %s\n", libusb_error_name(r)); }
        resp += rtl_rx_pump(h, f, 150);   /* listen 150ms after each probe */
    }
    fclose(f);
    printf("\nErgebnis: TX ok=%ld  TX-Fehler=%ld  empfangene Frames waehrend Test=%ld\n",
           tx_ok, tx_err, resp);
    printf("Probe-Responses zaehlen:  tshark -r %s -Y 'wlan.fc.type_subtype==0x05' 2>/dev/null | wc -l\n", out);
    printf("                     oder: tcpdump -r %s 2>/dev/null | grep -i 'Probe Response' | wc -l\n", out);

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
