/*
 * rtl_led — Status-LED des RTL8812AU (USB).
 *
 * Auf der AWUS036ACH-Variante des Nutzers steuert REG_LEDCFG0 (0x4C) die LED
 * (per ledscan verifiziert: Phase B). Encoding aus rtl8812au_led.c, else-Zweig,
 * LED_PIN_LED0:
 *   an : REG_LEDCFG0 = (cfg & 0x70) | BIT5           (Bit3=0 -> LED an)
 *   aus: REG_LEDCFG0 = (cfg & 0x70) | BIT3 | BIT5    (Bit3=1 -> LED aus)
 * BIT5 = SW-LED-Control aktiv.
 */
#include "rtl_usb.h"

#define BIT(n) (1u << (n))
#define REG_LEDCFG0 0x004C

void rtl_led_on(libusb_device_handle *h) {
    uint8_t cfg = rtl_read8(h, REG_LEDCFG0, NULL);
    rtl_write8(h, REG_LEDCFG0, (uint8_t)((cfg & 0x70) | BIT(5)));
}

void rtl_led_off(libusb_device_handle *h) {
    uint8_t cfg = rtl_read8(h, REG_LEDCFG0, NULL);
    rtl_write8(h, REG_LEDCFG0, (uint8_t)((cfg & 0x70) | BIT(3) | BIT(5)));
}
