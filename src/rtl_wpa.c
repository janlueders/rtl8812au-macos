/*
 * rtl_wpa — WPA2-PSK-Verbindung (Auth + Assoc + 4-Way-Handshake) als Funktion.
 * Uebernommen aus dem verifizierten connect-Tool (msg3/CCMP live bestaetigt).
 */
#include "rtl_wpa.h"
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rf.h"
#include "rtl_rx.h"
#include "rtl_tx.h"
#include "rtl_ccmp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonKeyDerivation.h>

static uint8_t w_sa[6];
static uint8_t w_bssid[6];

typedef struct {
    int got_m1, got_m3, got_auth, got_assoc, assoc_status, auth_status;
    uint8_t anonce[32], replay_m1[8], replay_m3[8];
    uint8_t m3_kd[256]; int m3_kdlen;
} hs_t;

static void prf384(const uint8_t pmk[32], const char *a, const uint8_t *b, int blen, uint8_t out[48]) {
    uint8_t buf[128], digest[20]; int alen = (int)strlen(a);
    for (int i = 0; i < 3; i++) {
        int p = 0; memcpy(buf, a, alen); p = alen; buf[p++] = 0x00;
        memcpy(buf + p, b, blen); p += blen; buf[p++] = (uint8_t)i;
        CCHmac(kCCHmacAlgSHA1, pmk, 32, buf, p, digest);
        memcpy(out + i * 20, digest, (i < 2) ? 20 : 8);
    }
}
static void amm(uint8_t *d, int *o, const uint8_t *x, const uint8_t *y, int n) {
    if (memcmp(x, y, n) < 0) { memcpy(d+*o,x,n);*o+=n; memcpy(d+*o,y,n);*o+=n; }
    else { memcpy(d+*o,y,n);*o+=n; memcpy(d+*o,x,n);*o+=n; }
}

static void cb(const uint8_t *f, uint32_t len, void *v) {
    hs_t *s = (hs_t *)v;
    if (len < 24) return;
    uint8_t fc0 = f[0];
    if ((fc0 == 0xB0 || fc0 == 0x10) && !memcmp(f+10,w_bssid,6) && !memcmp(f+4,w_sa,6)) {
        if (fc0 == 0xB0 && len >= 30) { s->got_auth=1; s->auth_status=f[28]|(f[29]<<8); }
        if (fc0 == 0x10 && len >= 30) { s->got_assoc=1; s->assoc_status=f[26]|(f[27]<<8); }
        return;
    }
    int is_data = ((fc0 & 0x0C) == 0x08); if (!is_data) return;
    int qos = ((fc0 & 0xF0) == 0x80); int hdr = 24 + (qos ? 2 : 0);
    if ((int)len < hdr + 8 + 95) return;
    if (memcmp(f+4,w_sa,6) || memcmp(f+10,w_bssid,6)) return;
    const uint8_t *llc = f + hdr;
    if (!(llc[0]==0xAA&&llc[1]==0xAA&&llc[2]==0x03&&llc[6]==0x88&&llc[7]==0x8E)) return;
    const uint8_t *e = llc + 8; if (e[1] != 0x03) return;
    uint16_t ki = (e[5]<<8)|e[6]; int mic=(ki&0x0100)!=0, ack=(ki&0x0080)!=0, sec=(ki&0x0200)!=0;
    if (ack && !mic) { memcpy(s->anonce,e+17,32); memcpy(s->replay_m1,e+9,8); s->got_m1=1; }
    else if (ack && mic && sec) {
        memcpy(s->replay_m3,e+9,8);
        int kdl=(e[97]<<8)|e[98];
        if (kdl>0 && kdl<=256 && (e+99+kdl)<=(f+len)) { memcpy(s->m3_kd,e+99,kdl); s->m3_kdlen=kdl; }
        s->got_m3=1;
    }
}

static int send_eapol(libusb_device_handle *h, uint16_t ki, const uint8_t rep[8],
                      const uint8_t *snonce, const uint8_t *kd, int kdl, const uint8_t kck[16]) {
    uint8_t fr[256]; int p=0;
    fr[p++]=0x08;fr[p++]=0x01;fr[p++]=0;fr[p++]=0;
    memcpy(fr+p,w_bssid,6);p+=6; memcpy(fr+p,w_sa,6);p+=6; memcpy(fr+p,w_bssid,6);p+=6;
    fr[p++]=0;fr[p++]=0;
    uint8_t snap[]={0xAA,0xAA,0x03,0,0,0,0x88,0x8E}; memcpy(fr+p,snap,8);p+=8;
    int e0=p; fr[p++]=0x02;fr[p++]=0x03; int lp=p; fr[p++]=0;fr[p++]=0; fr[p++]=0x02;
    fr[p++]=(ki>>8)&0xff; fr[p++]=ki&0xff; fr[p++]=0;fr[p++]=0x10;
    memcpy(fr+p,rep,8);p+=8;
    if(snonce)memcpy(fr+p,snonce,32);else memset(fr+p,0,32); p+=32;
    memset(fr+p,0,16);p+=16; memset(fr+p,0,8);p+=8; memset(fr+p,0,8);p+=8;
    int mp=p; memset(fr+p,0,16);p+=16;
    fr[p++]=(kdl>>8)&0xff; fr[p++]=kdl&0xff; if(kdl){memcpy(fr+p,kd,kdl);p+=kdl;}
    int el=p-(e0+4); fr[lp]=(el>>8)&0xff; fr[lp+1]=el&0xff;
    uint8_t dig[20]; CCHmac(kCCHmacAlgSHA1,kck,16,fr+e0,p-e0,dig); memcpy(fr+mp,dig,16);
    return rtl_tx_inject(h,fr,p,RTL_RATE_6M,RTL_QSLT_VO,RTL_TX_EP_MGMT);
}

int rtl_wpa_connect(libusb_device_handle *h, int channel,
                    const uint8_t bssid[6], const char *ssid, const char *psk,
                    wpa_keys_t *k, int verbose) {
    /* eigene MAC aus efuse waere schoener; hier fest wie im Rest des Projekts. */
    static const uint8_t sa[6] = { 0x00,0xc0,0xca,0xbc,0x4e,0xfa };
    memcpy(w_sa, sa, 6); memcpy(w_bssid, bssid, 6);
    memcpy(k->sa, sa, 6); memcpy(k->bssid, bssid, 6); k->have_gtk = 0;

    int slen = (int)strlen(ssid); if (slen > 32) slen = 32;
    uint8_t pmk[32];
    CCKeyDerivationPBKDF(kCCPBKDF2, psk, strlen(psk), (const uint8_t*)ssid, slen,
                         kCCPRFHmacAlgSHA1, 4096, pmk, 32);

    if (rtl_hal_full_init(h, channel, 0, 0) != 0) return -1;
    rtl_reg_write(h, REG_MACID, w_sa, 6);

    /* Clear any stale association the AP may still hold for our MAC from a
     * previous run: send a few deauth frames (reason 3). Unencrypted mgmt. */
    { uint8_t da[26]={0xC0,0x00,0,0};
      memcpy(da+4,w_bssid,6); memcpy(da+10,w_sa,6); memcpy(da+16,w_bssid,6);
      da[22]=0; da[23]=0; da[24]=0x03; da[25]=0x00;  /* reason: STA leaving */
      for (int i=0;i<3;i++){ rtl_tx_inject(h,da,26,RTL_RATE_6M,RTL_QSLT_MGNT,RTL_TX_EP_MGMT); usleep(20000); }
      usleep(150000); }

    hs_t s; memset(&s, 0, sizeof(s));

    uint8_t auth[30]={0xB0,0,0,0}; memcpy(auth+4,w_bssid,6);memcpy(auth+10,w_sa,6);memcpy(auth+16,w_bssid,6);
    auth[24]=0;auth[25]=0;auth[26]=1;auth[27]=0;auth[28]=0;auth[29]=0;
    for(int i=0;i<8&&!s.got_auth;i++){rtl_tx_inject(h,auth,30,RTL_RATE_6M,RTL_QSLT_MGNT,RTL_TX_EP_MGMT);rtl_rx_poll(h,150,cb,&s);}
    if(!s.got_auth||s.auth_status!=0){if(verbose)printf("[wpa] Auth fehlgeschlagen (%d)\n",s.auth_status);return -2;}

    uint8_t rsn[]={0x30,0x14,0x01,0,0,0x0f,0xac,0x04,0x01,0,0,0x0f,0xac,0x04,0x01,0,0,0x0f,0xac,0x02,0,0};
    uint8_t as[128]; int p=0;
    as[p++]=0;as[p++]=0;as[p++]=0;as[p++]=0;memcpy(as+p,w_bssid,6);p+=6;memcpy(as+p,w_sa,6);p+=6;memcpy(as+p,w_bssid,6);p+=6;
    as[p++]=0;as[p++]=0;as[p++]=0x11;as[p++]=0;as[p++]=0x0a;as[p++]=0;
    as[p++]=0;as[p++]=(uint8_t)slen;memcpy(as+p,ssid,slen);p+=slen;
    {uint8_t r[]={0x01,0x08,0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24};memcpy(as+p,r,10);p+=10;}
    {uint8_t er[]={0x32,0x04,0x30,0x48,0x60,0x6c};memcpy(as+p,er,6);p+=6;}
    memcpy(as+p,rsn,sizeof(rsn));p+=sizeof(rsn);
    for(int i=0;i<8&&!s.got_assoc;i++){rtl_tx_inject(h,as,p,RTL_RATE_6M,RTL_QSLT_MGNT,RTL_TX_EP_MGMT);rtl_rx_poll(h,150,cb,&s);}
    if(!s.got_assoc||s.assoc_status!=0){if(verbose)printf("[wpa] Assoc fehlgeschlagen (%d)\n",s.assoc_status);return -3;}

    for(int i=0;i<20&&!s.got_m1;i++)rtl_rx_poll(h,150,cb,&s);
    if(!s.got_m1){if(verbose)printf("[wpa] keine msg1\n");return -4;}

    uint8_t snonce[32]; for(int i=0;i<32;i++)snonce[i]=(uint8_t)arc4random();
    uint8_t b[76]; int bo=0; amm(b,&bo,w_bssid,w_sa,6); amm(b,&bo,s.anonce,snonce,32);
    uint8_t ptk[48]; prf384(pmk,"Pairwise key expansion",b,bo,ptk);
    memcpy(k->kck,ptk,16); memcpy(k->kek,ptk+16,16); memcpy(k->tk,ptk+32,16);

    if(send_eapol(h,0x010A,s.replay_m1,snonce,rsn,sizeof(rsn),k->kck)!=0)return -5;
    for(int i=0;i<20&&!s.got_m3;i++)rtl_rx_poll(h,150,cb,&s);
    if(!s.got_m3){if(verbose)printf("[wpa] keine msg3 (Passwort?)\n");return -6;}
    send_eapol(h,0x030A,s.replay_m3,NULL,NULL,0,k->kck);

    /* GTK aus msg3 auspacken */
    if(s.m3_kdlen>=24 && (s.m3_kdlen%8)==0){
        uint8_t kd[256];
        if(rtl_aes_unwrap(k->kek,s.m3_kd,s.m3_kdlen,kd)==0){
            int plen=s.m3_kdlen-8,i=0;
            while(i+2<=plen){int id=kd[i],l=kd[i+1]; if(l==0||i+2+l>plen)break;
                if(id==0xDD&&l>=6&&kd[i+2]==0&&kd[i+3]==0x0f&&kd[i+4]==0xac&&kd[i+5]==0x01){memcpy(k->gtk,kd+i+8,16);k->have_gtk=1;}
                i+=2+l;}
        }
    }
    /* Station mode: set network type = infrastructure (MSR) and the BSSID, so the
     * hardware auto-ACKs unicast frames addressed to us. Without this the chip
     * stays in NOLINK/monitor and never ACKs, so the AP drops our unicast replies
     * (e.g. ping/ICMP echo replies) -> the data path only worked for broadcast. */
    { uint8_t msr = rtl_read8(h, 0x0102, NULL);          /* MSR = REG_CR+2 */
      rtl_write8(h, 0x0102, (uint8_t)((msr & 0xFC) | 0x02)); } /* port0 = _HW_STATE_STATION_ */
    rtl_reg_write(h, 0x0618, w_bssid, 6);                /* REG_BSSID */

    if(verbose)printf("[wpa] connected, TK set, GTK %s, station mode (auto-ACK on).\n",
                      k->have_gtk?"ok":"missing");
    return 0;
}
