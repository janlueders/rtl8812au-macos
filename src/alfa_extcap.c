/*
 * alfa-extcap — Wireshark extcap-Anbindung fuer den Alfa AWUS036ACH.
 *
 * Damit erscheint der Adapter direkt in Wiresharks Interface-Liste. Wireshark
 * ruft dieses Programm mit dem extcap-Protokoll auf:
 *   --extcap-interfaces                -> Interfaces auflisten
 *   --extcap-dlts     --extcap-interface alfa0
 *   --extcap-config   --extcap-interface alfa0
 *   --capture --extcap-interface alfa0 --fifo <pfad> [--channel N]
 *
 * Installation: Binary in Wiresharks extcap-Verzeichnis kopieren, z.B.
 *   ~/.config/wireshark/extcap/   oder  /opt/homebrew/lib/wireshark/extcap/
 *
 * Bei --capture: volle Inbetriebnahme (RX-only, sicher) + Endlos-Capture in den
 * FIFO, bis Wireshark stoppt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"

#define IFACE "alfa0"

static const int chans_24[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const int chans_5g[] = {36,40,44,48,52,56,60,64,100,104,108,112,116,132,136,140,149,153,157,161,165};

static const char *argval(int argc, char **argv, const char *key) {
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], key)) return argv[i+1];
    return NULL;
}
static int hasarg(int argc, char **argv, const char *key) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], key)) return 1;
    return 0;
}

static void print_interfaces(void) {
    printf("extcap {version=1.0}{help=https://github.com/janlueders/rtl8812au-macos}\n");
    printf("interface {value=%s}{display=Alfa AWUS036ACH (RTL8812AU Monitor)}\n", IFACE);
}
static void print_dlts(void) {
    printf("dlt {number=127}{name=IEEE802_11_RADIOTAP}{display=802.11 plus radiotap header}\n");
}
static void print_config(void) {
    printf("arg {number=0}{call=--channel}{display=Kanal}{tooltip=WLAN-Kanal}{type=selector}{default=6}\n");
    for (size_t i = 0; i < sizeof(chans_24)/sizeof(int); i++)
        printf("value {arg=0}{value=%d}{display=%d (2.4 GHz)}\n", chans_24[i], chans_24[i]);
    for (size_t i = 0; i < sizeof(chans_5g)/sizeof(int); i++)
        printf("value {arg=0}{value=%d}{display=%d (5 GHz)}\n", chans_5g[i], chans_5g[i]);
}

static int do_capture(const char *fifo, int channel) {
    signal(SIGPIPE, SIG_IGN);   /* FIFO-Schliessen als Schreibfehler, nicht Kill */

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc || !h) { fprintf(stderr, "alfa-extcap: kein RTL8812AU gefunden\n"); libusb_exit(ctx); return 2; }

    if (rtl_hal_full_init(h, channel, 0, 0) != 0) {
        fprintf(stderr, "alfa-extcap: Init fehlgeschlagen\n");
        if (claimed) libusb_release_interface(h, 0);
        libusb_close(h); libusb_exit(ctx); return 3;
    }

    FILE *f = fopen(fifo, "wb");
    if (!f) { perror("alfa-extcap: fifo"); if (claimed) libusb_release_interface(h,0); libusb_close(h); libusb_exit(ctx); return 4; }
    rtl_rx_write_pcap_header(f);
    rtl_rx_stream(h, f);        /* laeuft bis Wireshark den FIFO schliesst */
    fclose(f);

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}

int main(int argc, char **argv) {
    if (hasarg(argc, argv, "--extcap-interfaces")) { print_interfaces(); return 0; }
    if (hasarg(argc, argv, "--extcap-dlts"))       { print_dlts();       return 0; }
    if (hasarg(argc, argv, "--extcap-config"))     { print_config();     return 0; }
    if (hasarg(argc, argv, "--capture")) {
        const char *fifo = argval(argc, argv, "--fifo");
        const char *chs  = argval(argc, argv, "--channel");
        if (!fifo) { fprintf(stderr, "alfa-extcap: --fifo fehlt\n"); return 1; }
        return do_capture(fifo, chs ? atoi(chs) : 6);
    }
    /* Ohne bekannte Option: Kurzhilfe. */
    print_interfaces();
    return 0;
}
