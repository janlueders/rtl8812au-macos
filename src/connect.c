/*
 * connect — IE3: WPA2-PSK 4-Way-Handshake (EAPOL) nach der Assoziierung.
 *
 * Ablauf: Open-Auth -> Assoc (mit RSN) -> EAPOL-4-Way:
 *   msg1 (AP: ANonce) -> PMK(PBKDF2) + PTK(PRF) -> msg2 (SNonce+MIC)
 *   -> msg3 (AP, MIC-geprueft) -> msg4. Erfolg = wir empfangen msg3
 *   (nur bei korrektem Passwort sendet der AP msg3).
 *
 * Nutzung: ./connect <kanal> <bssid> <ssid>
 * Das WLAN-Passwort wird per getpass abgefragt (nie in argv/History/Log).
 *
 * Krypto: macOS CommonCrypto (PBKDF2-SHA1, HMAC-SHA1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rf.h"
#include "rtl_rx.h"
#include "rtl_tx.h"
#include "rtl_ccmp.h"

static uint8_t g_sa[6] = { 0x00, 0xc0, 0xca, 0xbc, 0x4e, 0xfa };
static uint8_t g_bssid[6];

/* --- Handshake-Zustand aus dem RX-Callback --- */
typedef struct {
    int    got_m1, got_m3, got_auth, got_assoc, assoc_status, auth_status;
    uint8_t anonce[32];
    uint8_t replay_m1[8];
    uint8_t replay_m3[8];
    uint8_t m3_kd[256]; int m3_kdlen;   /* verschluesselte Key Data aus msg3 */
} hs_t;

/* CCMP-Live-Entschluesselungstest. */
static uint8_t g_gtk[16];
static uint8_t g_tk[16];
static int g_dec_ok, g_seen_prot, g_dec_fail;

static int parse_mac(const char *s, uint8_t *m) {
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6;
}

/* PRF-384 (WPA): out(48) = HMAC-SHA1(PMK, A||0x00||B||i) fuer i=0..2. */
static void prf384(const uint8_t pmk[32], const char *a,
                   const uint8_t *b, int blen, uint8_t out[48]) {
    uint8_t buf[128]; uint8_t digest[20];
    int alen = (int)strlen(a);
    for (int i = 0; i < 3; i++) {
        int p = 0;
        memcpy(buf + p, a, alen); p += alen;
        buf[p++] = 0x00;
        memcpy(buf + p, b, blen); p += blen;
        buf[p++] = (uint8_t)i;
        CCHmac(kCCHmacAlgSHA1, pmk, 32, buf, p, digest);
        int n = (i < 2) ? 20 : 8;         /* 48 = 20+20+8 */
        memcpy(out + i * 20, digest, n);
    }
}

/* min||max Vergleich fuer PTK-Ableitung. */
static void append_min_max(uint8_t *dst, int *o, const uint8_t *x, const uint8_t *y, int n) {
    if (memcmp(x, y, n) < 0) { memcpy(dst+*o, x, n); *o+=n; memcpy(dst+*o, y, n); *o+=n; }
    else                     { memcpy(dst+*o, y, n); *o+=n; memcpy(dst+*o, x, n); *o+=n; }
}

static void cb(const uint8_t *f, uint32_t len, void *v) {
    hs_t *s = (hs_t *)v;
    if (len < 24) return;
    uint8_t fc0 = f[0];

    /* Auth (0xB0) / Assoc-Resp (0x10) — Adressen: SA=f+10, DA=f+4 */
    if ((fc0 == 0xB0 || fc0 == 0x10) &&
        memcmp(f+10, g_bssid, 6) == 0 && memcmp(f+4, g_sa, 6) == 0) {
        if (fc0 == 0xB0 && len >= 30) { s->got_auth = 1; s->auth_status = f[28]|(f[29]<<8); }
        if (fc0 == 0x10 && len >= 30) { s->got_assoc = 1; s->assoc_status = f[26]|(f[27]<<8); }
        return;
    }

    /* Data (0x08) oder QoS-Data (0x88), vom AP an uns (fromDS): addr1=f+4=wir, addr2=f+10=BSSID */
    int is_data = ((fc0 & 0x0C) == 0x08);
    if (!is_data) return;
    int qos = ((fc0 & 0xF0) == 0x80);
    int hdr = 24 + (qos ? 2 : 0);
    if ((int)len < hdr + 8 + 95) return;
    if (memcmp(f+4, g_sa, 6) != 0 || memcmp(f+10, g_bssid, 6) != 0) return;
    const uint8_t *llc = f + hdr;
    if (!(llc[0]==0xAA && llc[1]==0xAA && llc[2]==0x03 && llc[6]==0x88 && llc[7]==0x8E)) return;

    const uint8_t *e = llc + 8;               /* EAPOL */
    if (e[1] != 0x03) return;                 /* EAPOL-Key */
    uint16_t ki = (e[5] << 8) | e[6];         /* Key Info */
    int mic = (ki & 0x0100) != 0, ack = (ki & 0x0080) != 0, secure = (ki & 0x0200) != 0;
    if (ack && !mic) {                         /* msg1 */
        memcpy(s->anonce, e + 17, 32);
        memcpy(s->replay_m1, e + 9, 8);
        s->got_m1 = 1;
    } else if (ack && mic && secure) {         /* msg3 */
        memcpy(s->replay_m3, e + 9, 8);
        int kdl = (e[97] << 8) | e[98];
        if (kdl > 0 && kdl <= 256 && (e + 99 + kdl) <= (f + len)) {
            memcpy(s->m3_kd, e + 99, kdl); s->m3_kdlen = kdl;
        }
        s->got_m3 = 1;
    }
}

/* RX-Callback fuer den CCMP-Test: geschuetzte Data-Frames vom AP entschluesseln.
 * Broadcast/Multicast -> GTK, Unicast an uns -> TK. */
static void dec_cb(const uint8_t *f, uint32_t len, void *v) {
    (void)v;
    if (len < 24) return;
    uint8_t fc0 = f[0], fc1 = f[1];
    if ((fc0 & 0x0C) != 0x08) return;          /* Data */
    if (!(fc1 & 0x40)) return;                  /* Protected */
    if (memcmp(f + 10, g_bssid, 6) != 0) return;/* vom AP (fromDS: addr2=BSSID) */
    int bcast = (f[4] & 0x01);
    int to_us = (memcmp(f + 4, g_sa, 6) == 0);
    if (!bcast && !to_us) return;
    g_seen_prot++;
    const uint8_t *key = bcast ? g_gtk : g_tk;
    uint8_t out[2048]; int ol = 0;
    if (rtl_ccmp_decrypt_frame(key, f, (int)len, out, &ol) == 0 && ol >= 8 &&
        out[0]==0xAA && out[1]==0xAA && out[2]==0x03) {
        if (!g_dec_ok) {
            g_dec_ok = 1;
            printf("  CCMP-Entschluesselung OK (%s): LLC/SNAP EtherType %02x%02x, %d Byte Payload\n",
                   bcast ? "Broadcast/GTK" : "Unicast/TK", out[6], out[7], ol - 8);
        }
    } else {
        g_dec_fail++;
    }
}

/* Baut + sendet einen EAPOL-Key-Frame (msg2/msg4) als 802.11-Data-toDS. */
static int send_eapol(libusb_device_handle *h, uint16_t key_info,
                      const uint8_t replay[8], const uint8_t *snonce,
                      const uint8_t *key_data, int kd_len, const uint8_t kck[16]) {
    uint8_t fr[256]; int p = 0;
    /* 802.11 data header (toDS) */
    fr[p++]=0x08; fr[p++]=0x01; fr[p++]=0x00; fr[p++]=0x00;
    memcpy(fr+p, g_bssid,6); p+=6; memcpy(fr+p, g_sa,6); p+=6; memcpy(fr+p, g_bssid,6); p+=6;
    fr[p++]=0x00; fr[p++]=0x00;                 /* seq */
    /* LLC/SNAP EAPOL */
    uint8_t snap[]={0xAA,0xAA,0x03,0x00,0x00,0x00,0x88,0x8E}; memcpy(fr+p,snap,8); p+=8;
    /* EAPOL-Key */
    int e0 = p;
    fr[p++]=0x02; fr[p++]=0x03;                 /* version 2, type Key */
    int lenpos = p; fr[p++]=0x00; fr[p++]=0x00; /* EAPOL length (spaeter) */
    fr[p++]=0x02;                               /* descriptor type RSN */
    fr[p++]=(key_info>>8)&0xff; fr[p++]=key_info&0xff;
    fr[p++]=0x00; fr[p++]=0x10;                 /* key length 16 (CCMP) */
    memcpy(fr+p, replay, 8); p+=8;
    if (snonce) memcpy(fr+p, snonce, 32); else memset(fr+p,0,32); p+=32; /* nonce */
    memset(fr+p,0,16); p+=16;                   /* key IV */
    memset(fr+p,0,8);  p+=8;                    /* key RSC */
    memset(fr+p,0,8);  p+=8;                    /* key ID */
    int micpos = p; memset(fr+p,0,16); p+=16;   /* MIC (spaeter) */
    fr[p++]=(kd_len>>8)&0xff; fr[p++]=kd_len&0xff;
    if (kd_len) { memcpy(fr+p, key_data, kd_len); p+=kd_len; }

    int eapol_len = p - (e0 + 4);
    fr[lenpos] = (eapol_len>>8)&0xff; fr[lenpos+1] = eapol_len&0xff;

    /* MIC = HMAC-SHA1(KCK, EAPOL-Frame mit MIC=0)[0:16] */
    uint8_t dig[20];
    CCHmac(kCCHmacAlgSHA1, kck, 16, fr + e0, p - e0, dig);
    memcpy(fr + micpos, dig, 16);

    return rtl_tx_inject(h, fr, p, RTL_RATE_6M, RTL_QSLT_VO, RTL_TX_EP_MGMT);
}

/* Krypto-Selbsttest gegen den IEEE-802.11i-Testvektor (kein Geraet noetig). */
static int selftest(void) {
    uint8_t pmk[32];
    const uint8_t want[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
    CCKeyDerivationPBKDF(kCCPBKDF2, "password", 8, (const uint8_t*)"IEEE", 4,
                         kCCPRFHmacAlgSHA1, 4096, pmk, 32);
    int ok = memcmp(pmk, want, 32) == 0;
    printf("PBKDF2-Selbsttest (passphrase=password, ssid=IEEE): %s\n", ok ? "PASS" : "FAIL");
    printf("  PMK = "); for (int i=0;i<32;i++) printf("%02x", pmk[i]); printf("\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc < 4) { printf("Nutzung: %s <kanal> <bssid> <ssid>   |   %s --selftest\n", argv[0], argv[0]); return 1; }
    int channel = atoi(argv[1]);
    if (!parse_mac(argv[2], g_bssid)) { printf("BSSID ungueltig\n"); return 1; }
    const char *ssid = argv[3];
    int slen = (int)strlen(ssid); if (slen > 32) slen = 32;

    char *pw = getpass("WLAN-Passwort (bleibt lokal): ");
    if (!pw || !*pw) { printf("Kein Passwort.\n"); return 1; }

    /* PMK = PBKDF2-SHA1(psk, ssid, 4096, 32) */
    uint8_t pmk[32];
    CCKeyDerivationPBKDF(kCCPBKDF2, pw, strlen(pw), (const uint8_t*)ssid, slen,
                         kCCPRFHmacAlgSHA1, 4096, pmk, 32);
    memset(pw, 0, strlen(pw));   /* Passwort sofort aus dem Speicher loeschen */

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t pid=0; int claimed=0;
    if (rtl_open_first(ctx,&h,&pid,&claimed) || !h) { printf("Kein Geraet.\n"); libusb_exit(ctx); return 2; }
    if (rtl_hal_full_init(h, channel, 0, 0) != 0) { printf("Init fehlgeschlagen.\n"); goto done; }
    rtl_reg_write(h, REG_MACID, g_sa, 6);

    hs_t s; memset(&s, 0, sizeof(s));

    /* Auth (Open) */
    uint8_t auth[30]={0xB0,0x00,0,0}; memcpy(auth+4,g_bssid,6); memcpy(auth+10,g_sa,6); memcpy(auth+16,g_bssid,6);
    auth[24]=0;auth[25]=0;auth[26]=1;auth[27]=0;auth[28]=0;auth[29]=0;
    for (int i=0;i<8 && !s.got_auth;i++){ rtl_tx_inject(h,auth,30,RTL_RATE_6M,RTL_QSLT_MGNT,RTL_TX_EP_MGMT); rtl_rx_poll(h,150,cb,&s); }
    if (!s.got_auth || s.auth_status!=0){ printf("Auth fehlgeschlagen (status %d).\n", s.auth_status); goto done; }
    printf("Auth OK.\n");

    /* Assoc mit RSN */
    uint8_t rsn[]={0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00};
    uint8_t as[128]; int p=0;
    as[p++]=0;as[p++]=0;as[p++]=0;as[p++]=0; memcpy(as+p,g_bssid,6);p+=6;memcpy(as+p,g_sa,6);p+=6;memcpy(as+p,g_bssid,6);p+=6;
    as[p++]=0;as[p++]=0; as[p++]=0x11;as[p++]=0x00; as[p++]=0x0a;as[p++]=0x00;
    as[p++]=0x00;as[p++]=(uint8_t)slen; memcpy(as+p,ssid,slen);p+=slen;
    { uint8_t r[]={0x01,0x08,0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24}; memcpy(as+p,r,10);p+=10; }
    { uint8_t er[]={0x32,0x04,0x30,0x48,0x60,0x6c}; memcpy(as+p,er,6);p+=6; }
    memcpy(as+p,rsn,sizeof(rsn)); p+=sizeof(rsn);
    for (int i=0;i<8 && !s.got_assoc;i++){ rtl_tx_inject(h,as,p,RTL_RATE_6M,RTL_QSLT_MGNT,RTL_TX_EP_MGMT); rtl_rx_poll(h,150,cb,&s); }
    if (!s.got_assoc || s.assoc_status!=0){ printf("Assoc fehlgeschlagen (status %d).\n", s.assoc_status); goto done; }
    printf("Assoziiert. Warte auf EAPOL msg1 ...\n");

    /* msg1 abwarten */
    for (int i=0;i<20 && !s.got_m1;i++) rtl_rx_poll(h,150,cb,&s);
    if (!s.got_m1){ printf("Keine EAPOL msg1 erhalten (AP hat evtl. deauthed).\n"); goto done; }
    printf("msg1 empfangen (ANonce). Leite PTK ab ...\n");

    /* SNonce + PTK */
    uint8_t snonce[32];
    for (int i=0;i<32;i++) snonce[i]=(uint8_t)arc4random();
    uint8_t b[76]; int bo=0;
    append_min_max(b,&bo,g_bssid,g_sa,6);        /* min||max(AA,SPA) */
    append_min_max(b,&bo,s.anonce,snonce,32);    /* min||max(ANonce,SNonce) */
    uint8_t ptk[48]; prf384(pmk, "Pairwise key expansion", b, bo, ptk);
    const uint8_t *kck = ptk;                    /* KCK = PTK[0:16] */
    memcpy(g_tk, ptk + 32, 16);                  /* TK = PTK[32:48] fuer Unicast-CCMP */

    /* msg2: SNonce + MIC + RSN als key data */
    if (send_eapol(h, 0x010A, s.replay_m1, snonce, rsn, sizeof(rsn), kck) != 0)
        { printf("msg2-Sendefehler.\n"); goto done; }
    printf("msg2 gesendet. Warte auf msg3 ...\n");

    for (int i=0;i<20 && !s.got_m3;i++) rtl_rx_poll(h,150,cb,&s);
    if (!s.got_m3){ printf("Keine msg3 — Passwort vermutlich falsch (MIC abgelehnt).\n"); goto done; }
    printf("msg3 empfangen -> Passwort korrekt, PTK stimmt.\n");

    /* msg4: bestaetigen */
    send_eapol(h, 0x030A, s.replay_m3, NULL, NULL, 0, kck);
    printf("msg4 gesendet.\n==> IE3 OK: WPA2-4-Way-Handshake abgeschlossen.\n\n");

    /* --- IE4/IE5-Vorstufe: GTK auspacken + CCMP live entschluesseln --- */
    const uint8_t *kek = ptk + 16;
    if (s.m3_kdlen >= 24 && (s.m3_kdlen % 8) == 0) {
        uint8_t kd[256];
        if (rtl_aes_unwrap(kek, s.m3_kd, s.m3_kdlen, kd) == 0) {
            int plen = s.m3_kdlen - 8, i = 0, have = 0;
            /* Key Data mischt IEs (z.B. RSN 0x30) und KDEs (0xDD) — nicht abbrechen,
             * sondern jedes Element ueberspringen und nur die GTK-KDE herausziehen. */
            while (i + 2 <= plen) {
                int id = kd[i], l = kd[i+1];
                if (l == 0 || i + 2 + l > plen) break;    /* Ende/Padding */
                if (id == 0xDD && l >= 6 &&
                    kd[i+2]==0x00 && kd[i+3]==0x0f && kd[i+4]==0xac && kd[i+5]==0x01) {
                    memcpy(g_gtk, kd + i + 8, 16); have = 1; /* GTK KDE: OUI+type+keyid+rsvd, dann GTK */
                }
                i += 2 + l;
            }
            if (have) {
                printf("GTK aus msg3 ausgepackt. Teste CCMP-Entschluesselung live (~12s) ...\n");
                for (int t = 0; t < 60 && !g_dec_ok; t++) rtl_rx_poll(h, 200, dec_cb, NULL);
                printf("  Diagnose: geschuetzte Frames vom AP gesehen=%d, Entschluessel-Fehler=%d\n",
                       g_seen_prot, g_dec_fail);
                if (g_dec_ok)
                    printf("\n==> IE4 live bestaetigt: CCMP-Rahmung korrekt, echtes Frame entschluesselt.\n");
                else if (g_seen_prot == 0)
                    printf("  Keine geschuetzten Frames vom AP empfangen (ruhiges Netz / evtl. deauthed).\n");
                else
                    printf("  Frames kamen an, aber Entschluesselung schlug fehl -> AAD/Nonce/GTK justieren.\n");
            } else printf("  Keine GTK-KDE in msg3 gefunden.\n");
        } else printf("  GTK-Unwrap fehlgeschlagen (KEK/Key-Data).\n");
    }
    printf("\nNaechster Schritt IE5: utun-Bridge + DHCP (Daemon, braucht sudo).\n");

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
