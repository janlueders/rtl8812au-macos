/*
 * ledtest — sichtbarer Beweis der Hardware-Kontrolle: die Status-LED blinken.
 *
 * Ablauf: oeffnen -> Power-On -> Firmware -> LED0 N-mal an/aus schalten.
 * Du solltest die LED am Adapter physisch blinken sehen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "rtl_usb.h"

int main(int argc, char **argv) {
    int blinks = (argc > 1) ? atoi(argv[1]) : 10;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein RTL8812AU gefunden. Adapter einstecken.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen fehlgeschlagen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Geraet 0bda:%04x, Claim: %s\n", pid, claimed ? "OK" : "NICHT");

    printf("Power-On: %s\n", rtl_power_on(h, 0) == 0 ? "OK" : "FEHLER");
    printf("Firmware: %s\n", rtl_fw_download(h, 0) == 0 ? "OK (WINTINI_RDY)" : "FEHLER");

    printf("\nLED blinkt %dx — schau auf den Adapter:\n", blinks);
    for (int i = 0; i < blinks; i++) {
        rtl_led_on(h);  printf("  [%2d] AN\n",  i + 1); fflush(stdout); usleep(300000);
        rtl_led_off(h); printf("  [%2d] AUS\n", i + 1); fflush(stdout); usleep(300000);
    }
    rtl_led_on(h); /* am Ende an lassen */
    printf("\nFertig. LED bleibt an.\n");

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
