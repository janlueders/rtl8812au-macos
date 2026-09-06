/*
 * rtl_led — Status-LED0 des RTL8812AU (USB), SW-Control.
 *
 * Portiert aus hal/rtl8812a/usb/rtl8812au_led.c (SwLedOn/Off_8812AU,
 * USB-Solo-Zweig, LED_PIN_LED0):
 *   an : REG_LEDCFG2 = (cfg & 0xf0) | BIT5 | BIT6   (Bit3=0 -> LED an)
 *   aus: REG_LEDCFG2 =  cfg | BIT3 | BIT5 | BIT6    (Bit3=1 -> LED aus)
 */
#include "rtl_usb.h"

#define BIT(n) (1u << (n))

void rtl_led_on(libusb_device_handle *h) {
    uint8_t cfg = rtl_read8(h, REG_LEDCFG2, NULL);
    rtl_write8(h, REG_LEDCFG2, (uint8_t)((cfg & 0xf0) | BIT(5) | BIT(6)));
}

void rtl_led_off(libusb_device_handle *h) {
    uint8_t cfg = rtl_read8(h, REG_LEDCFG2, NULL);
    rtl_write8(h, REG_LEDCFG2, (uint8_t)(cfg | BIT(3) | BIT(5) | BIT(6)));
}
