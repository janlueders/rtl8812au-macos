/*
 * associate — IE2: Authentifizierung (Open) + Assoziierung mit einem AP.
 *
 * Verifizierbar OHNE Passwort: Open-Auth und Assoc laufen vor dem WPA-Handshake.
 * Der AP antwortet mit Auth-Response und Assoc-Response (Statuscodes).
 *
 * Nutzung: ./associate <kanal> <bssid aa:bb:cc:dd:ee:ff> <ssid>
 * Beispiel: ./associate 6 04:b4:fe:81:91:cf "FRITZ!Box 6360 Cable"
 *
 * Setzt REG_MACID = unsere MAC, damit die Hardware Unicast-Antworten des AP
 * automatisch bestaetigt (ACK), sonst bricht der AP ab.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rf.h"
#include "rtl_rx.h"
#include "rtl_tx.h"

static uint8_t g_sa[6] = { 0x00, 0xc0, 0xca, 0xbc, 0x4e, 0xfa };
static uint8_t g_bssid[6];

typedef struct { int auth_status; int assoc_status; int aid; } assoc_ctx;

static int parse_mac(const char *s, uint8_t *m) {
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6;
}

static void cb(const uint8_t *f, uint32_t len, void *v) {
    assoc_ctx *c = (assoc_ctx *)v;
    if (len < 24) return;
    uint8_t fc = f[0];
    if (memcmp(f + 10, g_bssid, 6) != 0) return;   /* SA = Ziel-AP? */
    if (memcmp(f + 4, g_sa, 6) != 0) return;       /* DA = wir? */
    if (fc == 0xB0 && len >= 30) {                 /* Authentication */
        c->auth_status = f[28] | (f[29] << 8);     /* status bei body+4 */
    } else if (fc == 0x10 && len >= 30) {          /* Assoc Response */
        c->assoc_status = f[26] | (f[27] << 8);    /* status bei body+2 */
        c->aid = (f[28] | (f[29] << 8)) & 0x3fff;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("Nutzung: %s <kanal> <bssid> <ssid>\n", argv[0]); return 1; }
    int channel = atoi(argv[1]);
    if (!parse_mac(argv[2], g_bssid)) { printf("BSSID ungueltig\n"); return 1; }
    const char *ssid = argv[3];
    int slen = (int)strlen(ssid); if (slen > 32) slen = 32;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t pid = 0; int claimed = 0;
    int rc = rtl_open_first(ctx, &h, &pid, &claimed);
    if (rc || !h) { printf("Kein Geraet.\n"); libusb_exit(ctx); return 2; }
    if (rtl_hal_full_init(h, channel, 0, 0) != 0) { printf("Init fehlgeschlagen.\n"); goto done; }

    /* Eigene MAC ins MACID-Register -> Hardware-Auto-ACK fuer Antworten an uns. */
    rtl_reg_write(h, REG_MACID, g_sa, 6);

    assoc_ctx c = { -1, -1, 0 };

    /* --- Authentication (Open System) --- */
    uint8_t auth[30];
    uint8_t ah[] = { 0xB0,0x00, 0x00,0x00 };
    memcpy(auth, ah, 4);
    memcpy(auth+4, g_bssid, 6); memcpy(auth+10, g_sa, 6); memcpy(auth+16, g_bssid, 6);
    auth[22]=0; auth[23]=0;                 /* seq ctl */
    auth[24]=0x00; auth[25]=0x00;           /* algo = open */
    auth[26]=0x01; auth[27]=0x00;           /* transaction seq = 1 */
    auth[28]=0x00; auth[29]=0x00;           /* status */
    printf("Sende Authentication (Open) an %s ...\n", argv[2]);
    for (int i = 0; i < 8 && c.auth_status < 0; i++) {
        rtl_tx_inject(h, auth, sizeof(auth), RTL_RATE_6M, RTL_QSLT_MGNT, RTL_TX_EP_MGMT);
        rtl_rx_poll(h, 200, cb, &c);
    }
    if (c.auth_status < 0) { printf("Keine Auth-Response erhalten.\n"); goto done; }
    printf("Auth-Response: Status %d (%s)\n", c.auth_status, c.auth_status==0?"OK":"abgelehnt");
    if (c.auth_status != 0) goto done;

    /* --- Association Request --- */
    uint8_t assoc[128]; int p = 0;
    assoc[p++]=0x00; assoc[p++]=0x00;       /* FC: assoc req */
    assoc[p++]=0x00; assoc[p++]=0x00;       /* dur */
    memcpy(assoc+p, g_bssid, 6); p+=6; memcpy(assoc+p, g_sa, 6); p+=6; memcpy(assoc+p, g_bssid, 6); p+=6;
    assoc[p++]=0x00; assoc[p++]=0x00;       /* seq ctl */
    assoc[p++]=0x11; assoc[p++]=0x00;       /* cap: ESS|Privacy */
    assoc[p++]=0x0a; assoc[p++]=0x00;       /* listen interval */
    assoc[p++]=0x00; assoc[p++]=(uint8_t)slen; memcpy(assoc+p, ssid, slen); p+=slen; /* SSID IE */
    { uint8_t r[]={0x01,0x08,0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24}; memcpy(assoc+p,r,sizeof(r)); p+=sizeof(r); }
    { uint8_t er[]={0x32,0x04,0x30,0x48,0x60,0x6c}; memcpy(assoc+p,er,sizeof(er)); p+=sizeof(er); } /* Ext Rates */
    /* RSN-IE: WPA2, Group=CCMP, Pairwise=CCMP, AKM=PSK */
    { uint8_t rsn[]={0x30,0x14, 0x01,0x00, 0x00,0x0f,0xac,0x04,
                     0x01,0x00, 0x00,0x0f,0xac,0x04, 0x01,0x00, 0x00,0x0f,0xac,0x02, 0x00,0x00};
      memcpy(assoc+p,rsn,sizeof(rsn)); p+=sizeof(rsn); }
    printf("Sende Association Request (SSID \"%s\") ...\n", ssid);
    for (int i = 0; i < 8 && c.assoc_status < 0; i++) {
        rtl_tx_inject(h, assoc, p, RTL_RATE_6M, RTL_QSLT_MGNT, RTL_TX_EP_MGMT);
        rtl_rx_poll(h, 200, cb, &c);
    }
    if (c.assoc_status < 0) { printf("Keine Assoc-Response erhalten.\n"); goto done; }
    printf("Assoc-Response: Status %d (%s), AID=%d\n",
           c.assoc_status, c.assoc_status==0?"ASSOZIIERT":"abgelehnt", c.aid);
    if (c.assoc_status == 0)
        printf("\n==> IE2 OK: mit dem AP assoziiert. Naechster Schritt: WPA2-Handshake (IE3).\n");

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
