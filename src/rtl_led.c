/*
 * rtl_led — status LED of the RTL8812AU (USB).
 *
 * On the user's AWUS036ACH variant, REG_LEDCFG0 (0x4C) controls the LED
 * (verified via ledscan: phase B). Encoding from rtl8812au_led.c, else branch,
 * LED_PIN_LED0:
 *   on : REG_LEDCFG0 = (cfg & 0x70) | BIT5           (Bit3=0 -> LED on)
 *   off: REG_LEDCFG0 = (cfg & 0x70) | BIT3 | BIT5    (Bit3=1 -> LED off)
 * BIT5 = SW LED control active.
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
