/*
 * rtl_ccmp — AES-CCM (CCMP) + AES-Key-Unwrap. Siehe rtl_ccmp.c.
 */
#ifndef RTL_CCMP_H
#define RTL_CCMP_H

#include <stdint.h>

/* AES-CCM. encrypt=1: in=Klartext -> out=Cipher, mic[M] erzeugt.
 * encrypt=0: in=Cipher -> out=Klartext; wenn expected_mic gesetzt, pruefen
 * (Rueckgabe 0 ok, -1 MIC-Fehler). CCMP: nlen=13, M=8, L=2. */
int rtl_aes_ccm(int encrypt, const uint8_t key[16],
                const uint8_t *nonce, int nlen,
                const uint8_t *aad, int alen,
                const uint8_t *in, int inlen,
                uint8_t *out, uint8_t *mic, int M,
                const uint8_t *expected_mic);

/* RFC3394 AES-Key-Unwrap (GTK aus EAPOL msg3 mit KEK). 0 ok, -1 Fehler. */
int rtl_aes_unwrap(const uint8_t *kek, const uint8_t *wrapped, int wlen, uint8_t *out);

/* CCMP-Frame entschluesseln: vollstaendiges 802.11-Frame (Header + CCMP-Header
 * + Cipher + MIC) mit 16-Byte-Schluessel (TK unicast / GTK broadcast). Klartext
 * (LLC/SNAP + Payload) nach out, Laenge nach *outlen. 0 ok, -1 MIC-Fehler.
 * AAD/Nonce nach wpa_supplicant ccmp_aad_nonce. */
int rtl_ccmp_decrypt_frame(const uint8_t key[16], const uint8_t *frame, int len,
                           uint8_t *out, int *outlen);

/* CCMP-Frame bauen: 802.11-Header (hdr, hdrlen) + Payload -> out (komplettes
 * verschluesseltes Frame), *outlen gesetzt. pn = 48-bit Packet Number (wird vom
 * Aufrufer hochgezaehlt). 0 ok. */
int rtl_ccmp_encrypt_frame(const uint8_t key[16], const uint8_t *hdr, int hdrlen,
                           const uint8_t *payload, int plen, uint64_t pn,
                           uint8_t *out, int *outlen);

/* CCM-Kern-Selbsttest gegen NIST SP 800-38C. 0 = PASS. */
int rtl_ccmp_selftest(void);

#endif /* RTL_CCMP_H */
