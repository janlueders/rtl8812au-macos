/*
 * fwload — Milestone 2c: load firmware into the RTL8812AU.
 *
 * Success criterion: after the download the chip reports WINTINI_RDY (firmware
 * running). This is the native macOS equivalent of FirmwareDownload8812.
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
    if (pr != 0) { printf("Abbruch: ohne Power-On kein Firmware-Download.\n"); goto done; }

    printf("\nFirmware-Download:\n");
    rc = rtl_fw_download(h, 1);
    if (rc == 0)      printf("\n==> FIRMWARE LAEUFT. Chip voll initialisiert (bis auf PHY/RF).\n");
    else if (rc == 1) printf("\n==> Checksum fehlgeschlagen (Uebertragung).\n");
    else if (rc == 2) printf("\n==> Firmware nicht ready (WINTINI_RDY).\n");
    else              printf("\n==> USB-Fehler beim Download: %s\n", libusb_error_name(rc));

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return (pr == 0 && rc == 0) ? 0 : 1;
}
