/*
 * efuseinfo — Milestone 2b: read efuse and show the REAL MAC address.
 *
 * Flow: open device -> power-on (efuse macro available) -> decode the physical
 * efuse into the logical map -> print MAC from offset 0xD7.
 */
#include <stdio.h>
#include "rtl_usb.h"

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein RTL8812AU gefunden. Adapter einstecken.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen fehlgeschlagen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Geraet 0bda:%04x, Claim: %s\n", pid, claimed ? "OK" : "NICHT");

    int pr = rtl_power_on(h, 0);
    printf("Power-On: %s\n", pr == 0 ? "OK" : "fehlgeschlagen");

    static uint8_t map[EFUSE_MAP_LEN];
    rc = rtl_efuse_read_map(h, map, EFUSE_MAP_LEN);
    if (rc != 0) {
        printf("efuse-Read FEHLER: %s\n", libusb_error_name(rc));
    } else {
        const uint8_t *m = &map[EFUSE_MAC_OFFSET];
        printf("\nefuse-MAC (0x%02X) = %02x:%02x:%02x:%02x:%02x:%02x\n",
               EFUSE_MAC_OFFSET, m[0], m[1], m[2], m[3], m[4], m[5]);

        int all_ff = 1, all_00 = 1;
        for (int i = 0; i < 6; i++) { if (m[i] != 0xFF) all_ff = 0; if (m[i] != 0x00) all_00 = 0; }
        if (all_ff || all_00)
            printf("  ! unplausibel — efuse evtl. nicht bestromt; ggf. efuse-PowerSwitch noetig.\n");
        else
            printf("  plausible Hersteller-MAC gelesen.\n");

        printf("\nefuse-Kopf (0x00..0x1F):\n  ");
        for (int i = 0; i < 32; i++) { printf("%02x ", map[i]); if (i % 16 == 15) printf("\n  "); }
        printf("\n");
    }

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc == 0 ? 0 : 1;
}
