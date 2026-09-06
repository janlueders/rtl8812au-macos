/*
 * initchip — Milestone 2 (part 2): activate the chip via the power-on sequence.
 *
 * Success criterion: the POLLING steps (power-ready, MAC-on) complete.
 * This proves the chip executes our register writes and that its
 * hardware state machine responds — the native macOS equivalent of the Linux
 * "card enable".
 *
 * Note: the REAL MAC only comes after the efuse read (next step);
 * right after power-on, 0x0610 may still show the default.
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
    printf("Geraet 0bda:%04x geoeffnet, Claim: %s\n\n", pid, claimed ? "OK" : "NICHT");

    int r = 0;
    uint32_t sys = rtl_read32(h, REG_SYS_CFG, &r);
    printf("vor Power-On:  SYS_CFG=0x%08x (rc=%d)\n", sys, r);

    printf("\nPower-On-Sequenz (CARDEMU_TO_ACT):\n");
    int pr = rtl_power_on(h, 1);

    if (pr == 0)      printf("\n==> Power-On ERFOLGREICH. Chip ist aktiv.\n");
    else if (pr > 0)  printf("\n==> Power-On haengt bei POLLING-Schritt %d (State-Machine).\n", pr);
    else              printf("\n==> Power-On USB-Fehler: %s\n", libusb_error_name(pr));

    uint8_t mac[6] = {0};
    if (rtl_reg_read(h, REG_MACID, mac, 6) == 0)
        printf("MAC (0x0610) nach Power-On = %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return (pr == 0) ? 0 : 1;
}
