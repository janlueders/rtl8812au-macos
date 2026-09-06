/*
 * usbprobe — Meilenstein 1 fuer den nativen macOS-RTL8812AU-Zugriff.
 *
 * Findet den Alfa AWUS036ACH (Realtek VID 0x0bda) am USB, oeffnet ihn, dumpt
 * Descriptor + Endpoints (damit wir die Bulk-IN/OUT- und Control-Endpoints fuer
 * RX/TX/Register kennen) und versucht, das Interface zu claimen.
 *
 * Reiner Userspace, keine Kernel-/Kext-/DriverKit-Abhaengigkeit.
 *
 * Bauen:  make        (siehe Makefile)
 * Nutzen: Adapter einstecken, dann ./usbprobe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

#define REALTEK_VID 0x0bda

/* Bekannte RTL8812AU-PIDs (AWUS036ACH kommt meist als 0x8812 oder 0x881a). */
static const uint16_t known_pids[] = {
    0x8812, 0x881a, 0x881b, 0x881c, 0x8813, 0xa811, 0x0811, 0x0820, 0x0823,
};

static int is_known_pid(uint16_t pid) {
    for (size_t i = 0; i < sizeof(known_pids) / sizeof(known_pids[0]); i++)
        if (known_pids[i] == pid) return 1;
    return 0;
}

static const char *xfer_type(uint8_t attr) {
    switch (attr & LIBUSB_TRANSFER_TYPE_MASK) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:     return "control";
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
        case LIBUSB_TRANSFER_TYPE_BULK:        return "bulk";
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "interrupt";
        default:                               return "unknown";
    }
}

static void print_string(libusb_device_handle *h, uint8_t idx, const char *label) {
    if (!idx) return;
    unsigned char buf[256];
    int n = libusb_get_string_descriptor_ascii(h, idx, buf, sizeof(buf));
    if (n > 0) printf("  %-14s %s\n", label, buf);
}

static void dump_config(libusb_device *dev) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) {
        printf("  (keine aktive Konfiguration lesbar)\n");
        return;
    }
    printf("  Interfaces:    %d\n", cfg->bNumInterfaces);
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            printf("  [if %d alt %d] class=0x%02x sub=0x%02x proto=0x%02x eps=%d\n",
                   id->bInterfaceNumber, id->bAlternateSetting,
                   id->bInterfaceClass, id->bInterfaceSubClass,
                   id->bInterfaceProtocol, id->bNumEndpoints);
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                int in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                printf("      ep 0x%02x  %-4s  %-6s  maxpkt=%d\n",
                       ep->bEndpointAddress, in ? "IN" : "OUT",
                       xfer_type(ep->bmAttributes),
                       ep->wMaxPacketSize);
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

int main(void) {
    libusb_context *ctx = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        return 1;
    }

    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        fprintf(stderr, "get_device_list: %s\n", libusb_error_name((int)n));
        libusb_exit(ctx);
        return 1;
    }

    int found = 0;
    for (ssize_t i = 0; i < n; i++) {
        libusb_device *dev = list[i];
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(dev, &d) != 0) continue;
        if (d.idVendor != REALTEK_VID) continue;

        found++;
        int known = is_known_pid(d.idProduct);
        printf("\n=== Realtek-Geraet %04x:%04x  (bus %d, addr %d)%s ===\n",
               d.idVendor, d.idProduct,
               libusb_get_bus_number(dev), libusb_get_device_address(dev),
               known ? "  [bekannte RTL8812AU-PID]" : "  [PID nicht in Liste]");

        libusb_device_handle *h = NULL;
        rc = libusb_open(dev, &h);
        if (rc != 0) {
            printf("  open: %s (evtl. haelt macOS das Interface — Rechte/Claim)\n",
                   libusb_error_name(rc));
            dump_config(dev);
            continue;
        }

        print_string(h, d.iManufacturer, "Hersteller:");
        print_string(h, d.iProduct,      "Produkt:");
        print_string(h, d.iSerialNumber, "Seriennr.:");
        dump_config(dev);

        /* Claim-Versuch auf Interface 0 — belegt das Geraet fuer Userspace. */
        rc = libusb_claim_interface(h, 0);
        if (rc == 0) {
            printf("  claim if0:     OK — Userspace hat das Geraet.\n");
            libusb_release_interface(h, 0);
        } else {
            printf("  claim if0:     FEHLER: %s\n", libusb_error_name(rc));
            printf("                 (macOS haelt das Interface evtl. ueber einen\n");
            printf("                  Composite-Klassentreiber; naechster Schritt:\n");
            printf("                  IOKit-Detach / Kernel-Auto-Detach probieren.)\n");
        }
        libusb_close(h);
    }

    if (!found) {
        printf("Kein Realtek-Geraet (VID 0x0bda) am USB gefunden.\n");
        printf("Adapter einstecken und erneut ausfuehren. Gegenprobe:\n");
        printf("  system_profiler SPUSBDataType | grep -i realtek\n");
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found ? 0 : 2;
}
