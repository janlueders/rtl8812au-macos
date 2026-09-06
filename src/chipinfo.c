/*
 * chipinfo — Milestone 2 (part 1): prove that the chip responds to register
 * accesses. Reads REG_SYS_CFG (chip version) and the MAC address.
 *
 * This is the native macOS equivalent of the Linux "read_chip_version_8812a":
 * if the chip answers plausibly here, the USB register layer is working.
 */
#include <stdio.h>
#include "rtl_usb.h"

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        printf("Kein RTL8812AU (VID 0x0bda) gefunden. Adapter einstecken.\n");
        libusb_exit(ctx); return 2;
    }
    if (rc != 0 || !h) {
        fprintf(stderr, "Oeffnen fehlgeschlagen: %s\n", libusb_error_name(rc));
        libusb_exit(ctx); return 1;
    }

    printf("Geraet geoeffnet: 0bda:%04x   Interface-Claim: %s\n",
           pid, claimed ? "OK" : "NICHT geclaimt (Reads werden trotzdem versucht)");

    /* --- Chip version from SYS_CFG --- */
    int r = 0;
    uint32_t sys = rtl_read32(h, REG_SYS_CFG, &r);
    if (r != 0) {
        printf("\nSYS_CFG-Read FEHLER: %s\n", libusb_error_name(r));
        printf("Deutet darauf hin, dass der Interface-Claim/das Seizing noch fehlt\n");
        printf("(macOS-Klassentreiber) oder der Chip nicht mit Strom versorgt ist.\n");
    } else {
        printf("\nSYS_CFG (0x00F0) = 0x%08x\n", sys);
        printf("  Chip-Typ     : %s\n", (sys & SYS_CFG_RTL_ID) ? "TEST-Chip" : "MP (normal)");
        printf("  Vendor       : %s\n", (sys & SYS_CFG_VENDOR_ID) ? "UMC" : "TSMC");
        printf("  Cut-Version  : %u (roh, +1 fuer 8812)\n",
               (sys & SYS_CFG_CHIP_VER_MASK) >> SYS_CFG_CHIP_VER_SHIFT);
        if (sys == 0xffffffff || sys == 0x00000000)
            printf("  ! Wert unplausibel — vermutlich noch kein echter Register-Zugriff.\n");
    }

    /* --- MAC address --- */
    uint8_t mac[6] = {0};
    r = rtl_reg_read(h, REG_MACID, mac, 6);
    if (r == 0) {
        printf("\nMAC (0x0610) = %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        printf("\nMAC-Read FEHLER: %s\n", libusb_error_name(r));
    }

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return (r == 0) ? 0 : 1;
}
