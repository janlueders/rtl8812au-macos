/*
 * alfa-netd — IE5: Internet-Client-Daemon.
 *
 * Verbindet (WPA2), legt ein utun-Interface an, holt per DHCP eine IP und
 * brueckt IP-Pakete zwischen macOS (utun) und dem WLAN (802.11-Data + CCMP).
 *
 * Nutzung: sudo ./alfa-netd <kanal> <bssid> <ssid>
 * Passwort per getpass. Braucht root (utun + Routing). Aendert die Netzwerk-
 * Routen des Macs, solange es laeuft (Default-Route ueber das utun).
 *
 * ERSTE FASSUNG — wird an echtem Netz iterativ feinjustiert.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/kern_event.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rtl_usb.h"
#include "rtl_rx.h"
#include "rtl_tx.h"
#include "rtl_ccmp.h"
#include "rtl_wpa.h"

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

static wpa_keys_t K;
static uint64_t tx_pn = 1;
static int g_utun_fd = -1;
static uint8_t g_our_ip[4], g_gw_ip[4], g_mask[4], g_gw_mac[6];
static int g_have_gw_mac = 0;
/* DHCP-Erfassung waehrend des Setups. */
static int g_dhcp_mode = 0;
static uint8_t g_dtype = 0, g_yi[4], g_dmask[4], g_dgw[4], g_dsrv[4];
/* Diagnose-Zaehler. */
static long c_utun_out = 0, c_tx = 0, c_rx_ip = 0, c_rx_other = 0;

/* ---- utun ---- */
static int utun_open(char *ifname, size_t ilen) {
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) return -1;
    struct ctl_info ci; memset(&ci, 0, sizeof(ci));
    strncpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof(ci.ctl_name));
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) { close(fd); return -1; }
    struct sockaddr_ctl sc; memset(&sc, 0, sizeof(sc));
    sc.sc_len = sizeof(sc); sc.sc_family = AF_SYSTEM; sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = ci.ctl_id; sc.sc_unit = 0;   /* 0 = naechste freie utunN */
    if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) < 0) { close(fd); return -1; }
    socklen_t l = ilen;
    getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &l);
    return fd;
}

/* ---- 802.11-Data-Frame mit CCMP senden (toDS, DA=dst_mac, Ethertype) ---- */
static int send_l3(libusb_device_handle *h, const uint8_t dst_mac[6],
                   uint16_t ethertype, const uint8_t *payload, int plen) {
    uint8_t body[1600]; int p = 0;
    uint8_t snap[8] = {0xAA,0xAA,0x03,0,0,0, (uint8_t)(ethertype>>8), (uint8_t)ethertype};
    memcpy(body, snap, 8); p = 8;
    memcpy(body + p, payload, plen); p += plen;

    uint8_t hdr[24];
    hdr[0]=0x08; hdr[1]=0x01; hdr[2]=0; hdr[3]=0;      /* data, toDS */
    memcpy(hdr+4, K.bssid, 6); memcpy(hdr+10, K.sa, 6); memcpy(hdr+16, dst_mac, 6);
    hdr[22]=0; hdr[23]=0;

    uint8_t frame[1700]; int flen = 0;
    rtl_ccmp_encrypt_frame(K.tk, hdr, 24, body, p, tx_pn++, frame, &flen);
    return rtl_tx_inject(h, frame, flen, RTL_RATE_6M, RTL_QSLT_BE, RTL_TX_EP_MGMT);
}

/* ---- IPv4/UDP-Pruefsumme ---- */
static uint16_t csum16(const uint8_t *d, int n, uint32_t init) {
    uint32_t s = init;
    for (int i = 0; i + 1 < n; i += 2) s += (d[i] << 8) | d[i+1];
    if (n & 1) s += d[n-1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* ---- DHCP: DISCOVER/REQUEST bauen, OFFER/ACK parsen ---- */
static int dhcp_build(uint8_t *out, uint8_t msgtype, const uint8_t *xid,
                      const uint8_t *req_ip, const uint8_t *server_ip) {
    /* BOOTP + DHCP-Optionen */
    uint8_t bp[600]; memset(bp, 0, sizeof(bp)); int p = 0;
    bp[0]=1; bp[1]=1; bp[2]=6; bp[3]=0; p=4;      /* op,htype,hlen,hops */
    memcpy(bp+4, xid, 4); p=8;                     /* xid */
    p=28; memcpy(bp+28, K.sa, 6);                  /* chaddr */
    p=236; bp[236]=0x63; bp[237]=0x82; bp[238]=0x53; bp[239]=0x63; p=240; /* magic */
    bp[p++]=53; bp[p++]=1; bp[p++]=msgtype;        /* DHCP msg type */
    bp[p++]=55; bp[p++]=4; bp[p++]=1; bp[p++]=3; bp[p++]=6; bp[p++]=51; /* param req: mask,router,dns,lease */
    if (req_ip)   { bp[p++]=50; bp[p++]=4; memcpy(bp+p,req_ip,4); p+=4; }
    if (server_ip){ bp[p++]=54; bp[p++]=4; memcpy(bp+p,server_ip,4); p+=4; }
    bp[p++]=255;                                   /* end */
    int bootp_len = (p < 300) ? 300 : p;

    /* UDP (68->67) */
    uint8_t udp[8+600]; int ul = 8 + bootp_len;
    udp[0]=0;udp[1]=68; udp[2]=0;udp[3]=67; udp[4]=(ul>>8);udp[5]=ul&0xff; udp[6]=0;udp[7]=0;
    memcpy(udp+8, bp, bootp_len);

    /* IPv4 (0.0.0.0 -> 255.255.255.255) */
    int il = 20 + ul; int o = 0;
    out[o++]=0x45; out[o++]=0x00; out[o++]=(il>>8); out[o++]=il&0xff;
    out[o++]=0;out[o++]=0; out[o++]=0x40;out[o++]=0; out[o++]=64; out[o++]=17; /* TTL, proto UDP */
    out[o++]=0;out[o++]=0;                          /* hdr csum (spaeter) */
    memset(out+o,0,4); o+=4;                        /* src 0.0.0.0 */
    memset(out+o,0xff,4); o+=4;                     /* dst 255.255.255.255 */
    uint16_t ic = csum16(out, 20, 0); out[10]=ic>>8; out[11]=ic&0xff;
    memcpy(out+20, udp, ul);
    return il;
}

static int is_dhcp_reply(const uint8_t *ip, int len, uint8_t *out_type,
                         uint8_t *yiaddr, uint8_t *mask, uint8_t *gw, uint8_t *server) {
    if (len < 20 + 8 + 240) return 0;
    if (ip[9] != 17) return 0;                      /* UDP */
    const uint8_t *udp = ip + (ip[0]&0x0f)*4;
    if (!((udp[0]==0&&udp[1]==67)||(udp[2]==0&&udp[3]==68))) { /* 67->68 */ }
    if (udp[2]!=0 || udp[3]!=68) return 0;          /* dst port 68 */
    const uint8_t *bp = udp + 8;
    if (bp[236]!=0x63||bp[237]!=0x82||bp[238]!=0x53||bp[239]!=0x63) return 0;
    memcpy(yiaddr, bp+16, 4);                        /* yiaddr */
    const uint8_t *opt = bp+240; int n = len - (int)(opt-ip);
    int i=0; *out_type=0;
    while (i+1 < n) {
        int t=opt[i], l=opt[i+1]; if(t==255)break; if(t==0){i++;continue;}
        const uint8_t *v=opt+i+2;
        if (t==53&&l>=1) *out_type=v[0];
        else if (t==1&&l>=4) memcpy(mask,v,4);
        else if (t==3&&l>=4) memcpy(gw,v,4);
        else if (t==54&&l>=4) memcpy(server,v,4);
        i += 2 + l;
    }
    return *out_type != 0;
}

/* ---- ARP: Request bauen, Reply parsen ---- */
static void arp_request(libusb_device_handle *h, const uint8_t *target_ip) {
    uint8_t a[28]; int p=0;
    a[p++]=0;a[p++]=1; a[p++]=0x08;a[p++]=0x00; a[p++]=6;a[p++]=4; a[p++]=0;a[p++]=1; /* eth/ip, req */
    memcpy(a+p,K.sa,6);p+=6; memcpy(a+p,g_our_ip,4);p+=4;
    memset(a+p,0,6);p+=6; memcpy(a+p,target_ip,4);p+=4;
    uint8_t bc[6]; memset(bc,0xff,6);
    send_l3(h, bc, ETH_ARP, a, 28);
}

/* ---- RX-Dispatch ---- */
static void on_frame(const uint8_t *f, uint32_t len, void *v) {
    (void)v;
    if (len < 24) return;
    uint8_t fc0=f[0], fc1=f[1];
    if ((fc0 & 0x0C) != 0x08) return;               /* data */
    if (!(fc1 & 0x40)) return;                        /* protected */
    if (memcmp(f+10, K.bssid, 6) != 0) return;        /* fromDS: addr2=BSSID */
    int bcast = (f[4] & 0x01);
    int to_us = (memcmp(f+4, K.sa, 6) == 0);
    if (!bcast && !to_us) return;
    const uint8_t *key = bcast ? K.gtk : K.tk;
    uint8_t out[2048]; int ol=0;
    int elen = (int)len - 4;                          /* FCS */
    if (elen<=24 || rtl_ccmp_decrypt_frame(key, f, elen, out, &ol) != 0) return;
    if (ol < 8 || !(out[0]==0xAA&&out[1]==0xAA&&out[2]==0x03)) return;
    uint16_t et = (out[6]<<8)|out[7];
    const uint8_t *pl = out+8; int pll = ol-8;

    if (et == ETH_ARP && pll >= 28) {
        /* ARP-Reply auf unsere Anfrage? (op=2, sender-ip==gw) */
        if (out[8+6]==0 && out[8+7]==2) {
            if (!memcmp(pl+14, g_gw_ip, 4)) { memcpy(g_gw_mac, pl+8, 6); g_have_gw_mac=1; }
        }
    } else if (et == ETH_IP && pll >= 20) {
        if (g_dhcp_mode && is_dhcp_reply(pl, pll, &g_dtype, g_yi, g_dmask, g_dgw, g_dsrv))
            return;                                 /* DHCP-Antwort erfasst, nicht weiterleiten */
        if (g_utun_fd >= 0) {                        /* IP an macOS ueber utun */
            uint8_t buf[2100]; uint32_t af = htonl(AF_INET);
            memcpy(buf, &af, 4); memcpy(buf+4, pl, pll);
            (void)!write(g_utun_fd, buf, pll+4);
            c_rx_ip++;
        }
    } else {
        c_rx_other++;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("Nutzung: sudo %s <kanal> <bssid> <ssid>\n", argv[0]); return 1; }
    if (geteuid() != 0) { printf("Braucht root: sudo %s ...\n", argv[0]); return 1; }
    int channel = atoi(argv[1]);
    uint8_t bssid[6];
    if (sscanf(argv[2],"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",&bssid[0],&bssid[1],&bssid[2],&bssid[3],&bssid[4],&bssid[5])!=6)
        { printf("BSSID ungueltig\n"); return 1; }
    const char *ssid = argv[3];
    char *pw = getpass("WLAN-Passwort: "); if (!pw||!*pw) return 1;

    libusb_context *ctx=NULL; if (libusb_init(&ctx)) return 1;
    libusb_device_handle *h=NULL; uint16_t pid=0; int claimed=0;
    if (rtl_open_first(ctx,&h,&pid,&claimed)||!h){printf("Kein Geraet.\n");return 2;}

    printf("Verbinde mit WPA2 ...\n");
    int rc = rtl_wpa_connect(h, channel, bssid, ssid, pw, &K, 1);
    memset(pw,0,strlen(pw));
    if (rc != 0) { printf("Verbindung fehlgeschlagen (%d).\n", rc); goto done; }
    if (!K.have_gtk) printf("Warnung: kein GTK (Broadcast-RX eingeschraenkt).\n");
    printf("Verbunden. Lege utun an ...\n");

    char ifn[32]="utun9";
    g_utun_fd = utun_open(ifn, sizeof(ifn));
    if (g_utun_fd < 0) { printf("utun-Anlage fehlgeschlagen: %s\n", strerror(errno)); goto done; }
    fcntl(g_utun_fd, F_SETFL, O_NONBLOCK);
    printf("utun: %s\n", ifn);

    /* DHCP */
    printf("DHCP ...\n");
    uint8_t xid[4]={0xde,0xad,0xbe,0xef};
    uint8_t pkt[700]; int il;
    uint8_t bc[6]; memset(bc,0xff,6);
    g_dhcp_mode = 1;

    il = dhcp_build(pkt,1,xid,NULL,NULL);            /* DISCOVER -> OFFER (type 2) */
    for (int t=0;t<15 && g_dtype!=2;t++){ send_l3(h,bc,ETH_IP,pkt,il);
        for(int r=0;r<8 && g_dtype!=2;r++) rtl_rx_poll(h,150,on_frame,h); }
    if (g_dtype!=2){ printf("Kein DHCP-OFFER (Timing/Netz justieren).\n"); goto done; }
    memcpy(g_our_ip,g_yi,4); memcpy(g_mask,g_dmask,4); memcpy(g_gw_ip,g_dgw,4);
    printf("OFFER: IP %u.%u.%u.%u\n", g_yi[0],g_yi[1],g_yi[2],g_yi[3]);

    g_dtype=0;
    il = dhcp_build(pkt,3,xid,g_our_ip,g_dsrv);      /* REQUEST -> ACK (type 5) */
    for (int t=0;t<15 && g_dtype!=5;t++){ send_l3(h,bc,ETH_IP,pkt,il);
        for(int r=0;r<8 && g_dtype!=5;r++) rtl_rx_poll(h,150,on_frame,h); }
    if (g_dtype!=5){ printf("Kein DHCP-ACK.\n"); goto done; }
    g_dhcp_mode = 0;
    printf("ACK: IP %u.%u.%u.%u  GW %u.%u.%u.%u  Maske %u.%u.%u.%u\n",
           g_our_ip[0],g_our_ip[1],g_our_ip[2],g_our_ip[3],
           g_gw_ip[0],g_gw_ip[1],g_gw_ip[2],g_gw_ip[3],
           g_mask[0],g_mask[1],g_mask[2],g_mask[3]);

    /* Interface konfigurieren + Default-Route ueber utun. */
    char cmd[256];
    snprintf(cmd,sizeof(cmd),"ifconfig %s inet %u.%u.%u.%u %u.%u.%u.%u netmask 255.255.255.255 up",
        ifn, g_our_ip[0],g_our_ip[1],g_our_ip[2],g_our_ip[3], g_gw_ip[0],g_gw_ip[1],g_gw_ip[2],g_gw_ip[3]);
    printf("+ %s\n", cmd); system(cmd);
    snprintf(cmd,sizeof(cmd),"route -n add -net %u.%u.%u.%u/32 -interface %s",
        g_gw_ip[0],g_gw_ip[1],g_gw_ip[2],g_gw_ip[3], ifn); system(cmd);
    snprintf(cmd,sizeof(cmd),"route -n change default -interface %s 2>/dev/null || route -n add default -interface %s", ifn, ifn);
    printf("+ %s\n", cmd); system(cmd);

    /* Gateway-MAC per ARP aufloesen (fuer die 802.11-addr3 unserer TX). */
    printf("ARP Gateway ...\n");
    for (int t=0;t<10 && !g_have_gw_mac;t++){ arp_request(h,g_gw_ip);
        for(int r=0;r<6 && !g_have_gw_mac;r++) rtl_rx_poll(h,150,on_frame,h); }
    printf("Gateway-MAC: %s\n", g_have_gw_mac?"ok":"unbekannt (nutze Broadcast)");

    printf("\nBridge laeuft. Teste z.B.:  ping -c3 %u.%u.%u.%u\n",
           g_gw_ip[0],g_gw_ip[1],g_gw_ip[2],g_gw_ip[3]);
    printf("(Erste Fassung — Routing/DHCP-Feinschliff nach deinem Testlauf.)\n");
    uint8_t ub[2100];
    time_t last = time(NULL);
    for (;;) {
        rtl_rx_poll(h, 20, on_frame, NULL);
        for (;;) {                                  /* utun leerlesen, nicht nur 1 Paket */
            int n = (int)read(g_utun_fd, ub, sizeof(ub));
            if (n <= 4) break;
            const uint8_t *ip = ub + 4;             /* AF-Header ueberspringen */
            const uint8_t *dst = g_have_gw_mac ? g_gw_mac : bc;
            if (send_l3(h, dst, ETH_IP, ip, n - 4) == 0) c_tx++;
            c_utun_out++;
        }
        if (time(NULL) != last) {
            last = time(NULL);
            fprintf(stderr, "[stat] utun_out=%ld tx=%ld  rx_ip=%ld rx_other=%ld\n",
                    c_utun_out, c_tx, c_rx_ip, c_rx_other);
        }
    }

done:
    if (g_utun_fd>=0) close(g_utun_fd);
    if (claimed) libusb_release_interface(h,0);
    if (h) libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
