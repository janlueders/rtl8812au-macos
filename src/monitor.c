/*
 * monitor — Endziel M4: Monitor-Mode-Capture in eine pcap-Datei.
 *
 * Nutzung:  ./monitor [kanal] [sekunden] [ausgabe.pcap]
 * Standard: Kanal 6, 15 s, capture.pcap
 *
 * Ablauf: Geraet oeffnen -> volle Inbetriebnahme (rtl_hal_full_init) ->
 * 802.11-Frames von 0x81 lesen -> als pcap mit Radiotap schreiben.
 * Ergebnis in Wireshark/tshark oeffenbar.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"

int main(int argc, char **argv) {
    int channel   = (argc > 1) ? atoi(argv[1]) : 6;
    int seconds   = (argc > 2) ? atoi(argv[2]) : 15;
    const char *out = (argc > 3) ? argv[3] : "capture.pcap";

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein RTL8812AU gefunden. Adapter einstecken.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen fehlgeschlagen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Geraet 0bda:%04x, Claim: %s\n\n", pid, claimed ? "OK" : "NICHT");

    rc = rtl_hal_full_init(h, channel, 1);
    if (rc != 0) { printf("\nInbetriebnahme fehlgeschlagen (rc=%d). Abbruch.\n", rc); goto done; }

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); rc = 1; goto done; }
    rtl_rx_write_pcap_header(f);

    printf("\nMonitor auf Kanal %d, %d s -> %s\n", channel, seconds, out);
    long n = rtl_rx_capture(h, f, seconds, 1);
    fclose(f);
    printf("Fertig: %ld Frames geschrieben nach %s\n", n, out);
    printf("Oeffnen mit:  wireshark %s   oder   tshark -r %s\n", out, out);

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
