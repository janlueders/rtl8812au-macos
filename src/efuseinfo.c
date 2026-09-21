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

        /* PA/LNA type + RF board option (hal_pg.h): whether this exact unit
         * has an external power amplifier -- if so, and our RF/BB bring-up
         * never enables it (board_type hardcoded to "no PA/LNA" in
         * rtl_rf.c), we transmit through the chip's own weak internal driver
         * stage only, never through the external PA this adapter is built
         * around. PAType bit4+bit5 both set = external PA present (2G). */
        printf("\nThermal-Meter-Kalibrierwert (0xBA):\n  %02x", map[0xBA]);
        if (map[0xBA] == 0xFF) printf("  (unprogrammiert -- auch der echte Treiber ueberspringt Thermal-Tracking dann)");
        printf("\n");

        printf("\nPA/LNA-Typ (0xBC..0xC1):\n  ");
        for (int i = 0xBC; i <= 0xC1; i++) printf("%02x ", map[i]);
        printf("\n");
        uint8_t pa_type_2g = map[0xBC];
        int ext_pa_2g = (pa_type_2g != 0xFF) && (pa_type_2g & 0x30) == 0x30;
        printf("  PAType_2G=0x%02x -> externer PA (2.4GHz): %s\n",
               pa_type_2g, ext_pa_2g ? "JA" : "nein (laut Efuse)");

        /* RFE (RF Front-End) option (hal_pg.h EEPROM_RFE_OPTION_8812=0xCA):
         * selects which phy_SetRFEReg8812 pinmux case applies -- i.e. whether
         * the RF switch actually routes the signal through the external
         * PA/LNA at all. Our code hardcodes rfe_type=0 with a comment
         * claiming that's the AWUS036ACH default -- checking that here too. */
        uint8_t rfe_option = map[0xCA];
        printf("  RFE-Option (0xCA) = 0x%02x -> rfe_type = %u %s\n",
               rfe_option, (unsigned)(rfe_option & 0x3F),
               rfe_option == 0xFF ? "(unprogrammiert/Default)" : "");
    }

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc == 0 ? 0 : 1;
}
