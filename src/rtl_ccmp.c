/*
 * rtl_ccmp — AES-CCM (CCMP) fuer 802.11-Datenframes + AES-Key-Unwrap (RFC3394).
 *
 * Software-Krypto (macOS CommonCrypto, AES-128-ECB als Primitive), damit der
 * Datenpfad ohne Chip-HW-Krypto auskommt. CCM-Kern gegen NIST SP 800-38C
 * verifizierbar (rtl_ccmp_selftest).
 */
#include "rtl_ccmp.h"
#include <string.h>
#include <CommonCrypto/CommonCrypto.h>

static void aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    size_t moved = 0;
    CCCrypt(kCCEncrypt, kCCAlgorithmAES128, kCCOptionECBMode,
            key, 16, NULL, in, 16, out, 16, &moved);
}

/* AES-CCM. encrypt=1: in=plaintext(inlen) -> out=cipher(inlen), mic(M) erzeugt.
 * encrypt=0: in=cipher(inlen) -> out=plaintext(inlen), mic(M) gegen erwartet
 * pruefen (Rueckgabe 0 ok, -1 MIC-Fehler). nonce nlen=13 (CCMP), M=8, L=2. */
int rtl_aes_ccm(int encrypt, const uint8_t key[16],
                const uint8_t *nonce, int nlen,
                const uint8_t *aad, int alen,
                const uint8_t *in, int inlen,
                uint8_t *out, uint8_t *mic, int M,
                const uint8_t *expected_mic) {
    int L = 15 - nlen;
    uint8_t X[16], B[16], S0[16], A[16];

    /* Fuer Decrypt zuerst entschluesseln (MAC laeuft ueber Klartext). */
    /* CTR-Bloecke vorbereiten: A0 = [L-1][nonce][0..0] */
    memset(A, 0, 16);
    A[0] = (uint8_t)(L - 1);
    memcpy(A + 1, nonce, nlen);
    aes_ecb(key, A, S0);   /* S0 fuer MIC-Verschluesselung */

    uint8_t *plain = out;  /* out haelt am Ende Klartext (dec) bzw. Cipher (enc) */
    if (!encrypt) {
        /* CTR-Entschluesselung: cipher(in) -> plain(out) */
        for (int off = 0; off < inlen; off += 16) {
            uint32_t ctr = (uint32_t)(off / 16 + 1);
            memset(A, 0, 16); A[0] = (uint8_t)(L - 1); memcpy(A + 1, nonce, nlen);
            A[15] = ctr & 0xff; A[14] = (ctr >> 8) & 0xff;
            uint8_t S[16]; aes_ecb(key, A, S);
            int n = (inlen - off < 16) ? (inlen - off) : 16;
            for (int i = 0; i < n; i++) plain[off + i] = in[off + i] ^ S[i];
        }
    }
    const uint8_t *mac_src = encrypt ? in : plain;  /* MAC immer ueber Klartext */

    /* --- CBC-MAC --- */
    memset(B, 0, 16);
    B[0] = (uint8_t)((alen > 0 ? 0x40 : 0) | (((M - 2) / 2) << 3) | (L - 1));
    memcpy(B + 1, nonce, nlen);
    for (int i = 0; i < L; i++) B[15 - i] = (uint8_t)((inlen >> (8 * i)) & 0xff);
    aes_ecb(key, B, X);   /* X = E(B0), da X0=0 */

    if (alen > 0) {
        uint8_t ab[16]; memset(ab, 0, 16);
        ab[0] = (uint8_t)((alen >> 8) & 0xff); ab[1] = (uint8_t)(alen & 0xff);
        int first = alen < 14 ? alen : 14;
        memcpy(ab + 2, aad, first);
        for (int i = 0; i < 16; i++) X[i] ^= ab[i];
        aes_ecb(key, X, X);
        int rem = alen - first, off = first;
        while (rem > 0) {
            memset(ab, 0, 16); int n = rem < 16 ? rem : 16;
            memcpy(ab, aad + off, n); off += n; rem -= n;
            for (int i = 0; i < 16; i++) X[i] ^= ab[i];
            aes_ecb(key, X, X);
        }
    }
    for (int off = 0; off < inlen; off += 16) {
        uint8_t blk[16]; memset(blk, 0, 16);
        int n = (inlen - off < 16) ? (inlen - off) : 16;
        memcpy(blk, mac_src + off, n);
        for (int i = 0; i < 16; i++) X[i] ^= blk[i];
        aes_ecb(key, X, X);
    }
    /* T = X[0:M] XOR S0[0:M] */
    uint8_t T[16];
    for (int i = 0; i < M; i++) T[i] = X[i] ^ S0[i];

    if (encrypt) {
        for (int off = 0; off < inlen; off += 16) {
            uint32_t ctr = (uint32_t)(off / 16 + 1);
            memset(A, 0, 16); A[0] = (uint8_t)(L - 1); memcpy(A + 1, nonce, nlen);
            A[15] = ctr & 0xff; A[14] = (ctr >> 8) & 0xff;
            uint8_t S[16]; aes_ecb(key, A, S);
            int n = (inlen - off < 16) ? (inlen - off) : 16;
            for (int i = 0; i < n; i++) out[off + i] = in[off + i] ^ S[i];
        }
        memcpy(mic, T, M);
        return 0;
    } else {
        if (mic) memcpy(mic, T, M);
        if (expected_mic && memcmp(T, expected_mic, M) != 0) return -1;
        return 0;
    }
}

/* RFC3394 AES-Key-Unwrap (fuer GTK aus EAPOL msg3, KEK). out = wrapped-8 Bytes.
 * wlen = Laenge der gewickelten Daten (Vielfaches von 8, >= 24). */
int rtl_aes_unwrap(const uint8_t *kek, const uint8_t *wrapped, int wlen, uint8_t *out) {
    int n = wlen / 8 - 1;
    if (n < 1) return -1;
    uint8_t a[8], r[64][8], b[16];
    memcpy(a, wrapped, 8);
    for (int i = 1; i <= n; i++) memcpy(r[i], wrapped + 8 * i, 8);
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * j + i;
            memcpy(b, a, 8); memcpy(b + 8, r[i], 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)((t >> (8 * k)) & 0xff);
            uint8_t dec[16]; size_t moved = 0;
            CCCrypt(kCCDecrypt, kCCAlgorithmAES128, kCCOptionECBMode, kek, 16, NULL, b, 16, dec, 16, &moved);
            memcpy(a, dec, 8); memcpy(r[i], dec + 8, 8);
        }
    }
    for (int i = 1; i <= n; i++) memcpy(out + 8 * (i - 1), r[i], 8);
    /* Integritaet: a muss dem Default-IV A6A6A6A6A6A6A6A6 entsprechen. */
    for (int i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
    return 0;
}

/* AAD + Nonce aus dem 802.11-Header bauen (wpa_supplicant ccmp_aad_nonce).
 * ccmp = Zeiger auf den 8-Byte-CCMP-Header (nach dem MAC-Header). */
static int ccmp_aad_nonce(const uint8_t *f, int hdrlen, int qos, int a4,
                          const uint8_t *ccmp, uint8_t *aad, int *aadlen, uint8_t nonce[13]) {
    uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
    uint16_t stype = fc & 0x00F0;
    if ((fc & 0x000C) == 0x0008) {          /* Data */
        fc &= ~0x0070;                       /* Subtype-Bits 4,5,6 maskieren */
        if (stype & 0x0080) fc &= ~0x8000;   /* QoS -> Order maskieren */
    }
    fc &= ~0x0800; fc &= ~0x1000; fc &= ~0x2000; /* Retry, PwrMgmt, MoreData */
    /* Protected-Bit NICHT maskieren: der AP (mac80211) laesst es in der AAD auf 1. */

    int p = 0;
    aad[p++] = fc & 0xff; aad[p++] = (fc >> 8) & 0xff;
    memcpy(aad + p, f + 4, 18); p += 18;         /* A1 + A2 + A3 */
    aad[p++] = f[22] & 0x0f; aad[p++] = 0x00;    /* SC: Frag behalten, Seq maskieren */
    if (a4) { memcpy(aad + p, f + 24, 6); p += 6; }
    if (qos) { aad[p++] = f[hdrlen - 2] & 0x0f; aad[p++] = 0x00; }
    *aadlen = p;

    nonce[0] = (uint8_t)(qos ? (f[hdrlen - 2] & 0x0f) : 0);
    memcpy(nonce + 1, f + 10, 6);                /* A2 */
    nonce[7]=ccmp[7]; nonce[8]=ccmp[6]; nonce[9]=ccmp[5];
    nonce[10]=ccmp[4]; nonce[11]=ccmp[1]; nonce[12]=ccmp[0];
    return 0;
}

int rtl_ccmp_decrypt_frame(const uint8_t key[16], const uint8_t *f, int len,
                           uint8_t *out, int *outlen) {
    uint8_t fc0 = f[0], fc1 = f[1];
    int qos = ((fc0 & 0xF0) == 0x80);
    int a4  = ((fc1 & 0x03) == 0x03);
    int hdrlen = 24 + (a4 ? 6 : 0) + (qos ? 2 : 0);
    if (len < hdrlen + 8 + 8) return -1;
    const uint8_t *ccmp = f + hdrlen;
    if (!(ccmp[3] & 0x20)) return -1;            /* ExtIV muss gesetzt sein (CCMP) */

    uint8_t aad[32], nonce[13]; int al = 0;
    ccmp_aad_nonce(f, hdrlen, qos, a4, ccmp, aad, &al, nonce);

    int clen = len - hdrlen - 8 - 8;             /* minus CCMP-Header minus MIC */
    const uint8_t *cipher = f + hdrlen + 8;
    const uint8_t *mic = cipher + clen;
    if (clen <= 0) return -1;
    int r = rtl_aes_ccm(0, key, nonce, 13, aad, al, cipher, clen, out, NULL, 8, mic);
    if (r == 0) *outlen = clen;
    return r;
}

int rtl_ccmp_encrypt_frame(const uint8_t key[16], const uint8_t *hdr, int hdrlen,
                           const uint8_t *payload, int plen, uint64_t pn,
                           uint8_t *out, int *outlen) {
    memcpy(out, hdr, hdrlen);
    out[1] |= 0x40;                              /* Protected-Bit setzen */
    uint8_t *ccmp = out + hdrlen;
    ccmp[0] = pn & 0xff; ccmp[1] = (pn >> 8) & 0xff; ccmp[2] = 0x00;
    ccmp[3] = 0x20;                              /* ExtIV, KeyID 0 */
    ccmp[4] = (pn >> 16) & 0xff; ccmp[5] = (pn >> 24) & 0xff;
    ccmp[6] = (pn >> 32) & 0xff; ccmp[7] = (pn >> 40) & 0xff;

    int qos = ((out[0] & 0xF0) == 0x80);
    int a4  = ((out[1] & 0x03) == 0x03);
    uint8_t aad[32], nonce[13]; int al = 0;
    ccmp_aad_nonce(out, hdrlen, qos, a4, ccmp, aad, &al, nonce);

    uint8_t *cipher = out + hdrlen + 8;
    uint8_t mic[8];
    rtl_aes_ccm(1, key, nonce, 13, aad, al, payload, plen, cipher, mic, 8, NULL);
    memcpy(cipher + plen, mic, 8);
    *outlen = hdrlen + 8 + plen + 8;
    return 0;
}

/* Selbsttest: NIST SP 800-38C, Example 1 (M=4, L=8, nonce 7B). */
int rtl_ccmp_selftest(void) {
    uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = 0x40 + i;
    uint8_t N[7]  = {0x10,0x11,0x12,0x13,0x14,0x15,0x16};
    uint8_t A[8]  = {0,1,2,3,4,5,6,7};
    uint8_t P[4]  = {0x20,0x21,0x22,0x23};
    uint8_t want_c[4] = {0x71,0x62,0x01,0x5b};
    uint8_t want_t[4] = {0x4d,0xac,0x25,0x5d};
    uint8_t c[4], mic[4];
    rtl_aes_ccm(1, key, N, 7, A, 8, P, 4, c, mic, 4, NULL);
    int ok = (memcmp(c, want_c, 4) == 0) && (memcmp(mic, want_t, 4) == 0);
    return ok ? 0 : 1;
}
