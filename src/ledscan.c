/*
 * ledscan — LED-Diagnose nach voller Inbetriebnahme.
 *
 * Testet beide Kandidaten-Register getrennt, damit wir sehen, welches (falls
 * eines) die LED deiner AWUS036ACH-Variante steuert:
 *   Phase A: REG_LEDCFG2 (0x4E), LED0 — USB-Solo-Zweig
 *   Phase B: REG_LEDCFG0 (0x4C), LED0 — Alternativ-Zweig
 * Jede Phase blinkt 6x. Sag mir, in welcher Phase (falls) die LED blinkt.
 */
#include <stdio.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_hal.h"

#define BITn(n) (1u << (n))
#define LEDCFG0 0x004C
#define LEDCFG2 0x004E

static void blink(libusb_device_handle *h, uint16_t reg, int usb_solo) {
    for (int i = 0; i < 6; i++) {
        uint8_t c = rtl_read8(h, reg, NULL);
        if (usb_solo) rtl_write8(h, reg, (uint8_t)((c & 0xf0) | BITn(5) | BITn(6)));   /* an */
        else          rtl_write8(h, reg, (uint8_t)((c & 0x70) | BITn(5)));             /* an */
        usleep(350000);
        c = rtl_read8(h, reg, NULL);
        if (usb_solo) rtl_write8(h, reg, (uint8_t)(c | BITn(3) | BITn(5) | BITn(6)));  /* aus */
        else          rtl_write8(h, reg, (uint8_t)((c & 0x70) | BITn(3) | BITn(5)));   /* aus */
        usleep(350000);
    }
}

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc || !h) { printf("Kein Geraet.\n"); libusb_exit(ctx); return 2; }

    if (rtl_hal_full_init(h, 6, 0, 0) != 0) { printf("Init fehlgeschlagen.\n"); goto done; }
    printf("Init OK. Achte jetzt auf die LED.\n\n");

    printf(">>> Phase A: REG_LEDCFG2 (0x4E) — blinkt 6x ...\n"); fflush(stdout);
    blink(h, LEDCFG2, 1);
    sleep(1);
    printf(">>> Phase B: REG_LEDCFG0 (0x4C) — blinkt 6x ...\n"); fflush(stdout);
    blink(h, LEDCFG0, 0);
    printf("\nFertig. In welcher Phase hat die LED geblinkt (A, B, beide, keine)?\n");

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
