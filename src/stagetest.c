/*
 * stagetest — stufenweise, kontrollierte Inbetriebnahme fuer sicheres Testen.
 *
 * Nutzung: ./stagetest <stufe>
 *   1 = Power-On
 *   2 = + Firmware
 *   3 = + MAC-Init + LED an        (LED muss hier angehen; noch KEIN RF/TX)
 *   4 = + BB-Init
 *   5 = + RF-Init                  (RF-Pfad an, aber kein TX)
 *   6 = + LCK + IQK                (Kalibrierung; internes TX — hoechste Stufe)
 *   7 = + Kanal + Monitor-RCR
 *
 * Jede Stufe druckt ihren Rueckgabecode. Bricht bei Fehler ab.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rtl_usb.h"
#include "rtl_mac.h"
#include "rtl_bb.h"
#include "rtl_rf.h"
#include "rtl_cal.h"

#define RUN(cond, label, call) do { \
    if (stage >= (cond)) { \
        printf("[Stufe %d] %s ...\n", (cond), label); \
        int _rc = (call); \
        if (_rc != 0) { printf("   -> FEHLER rc=%d, Abbruch.\n", _rc); goto done; } \
        printf("   -> OK\n"); \
    } \
} while (0)

int main(int argc, char **argv) {
    int stage = (argc > 1) ? atoi(argv[1]) : 3;
    int channel = (argc > 2) ? atoi(argv[2]) : 6;
    int rc = 0;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }
    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein RTL8812AU gefunden.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Geraet 0bda:%04x, Claim: %s, Ziel-Stufe %d\n\n", pid, claimed ? "OK" : "NEIN", stage);

    RUN(1, "Power-On",          rtl_power_on(h, 0));
    RUN(2, "Firmware",          rtl_fw_download(h, 0));
    RUN(3, "MAC-Init",          rtl_mac_init(h, 1));
    if (stage >= 3) { rtl_led_on(h); printf("[Stufe 3] LED an -> schau auf den Adapter\n"); }
    RUN(4, "BB-Init",           rtl_bb_init(h, 1));
    RUN(5, "RF-Init",           rtl_rf_init(h, 1));
    RUN(6, "LC-Kalibrierung",   rtl_lck(h, 1));
    RUN(6, "IQ-Kalibrierung",   rtl_iqk(h, 1));
    RUN(7, "Kanal setzen",      rtl_rf_set_channel(h, channel, 0));
    RUN(7, "Monitor-RCR",       rtl_mac_set_monitor(h, 1));

    printf("\nStufe %d erreicht, alles OK.\n", stage);
done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
