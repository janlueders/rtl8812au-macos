/*
 * rtl_ccmp — AES-CCM (CCMP) + AES-Key-Unwrap. See rtl_ccmp.c.
 */
#ifndef RTL_CCMP_H
#define RTL_CCMP_H

#include <stdint.h>

/* AES-CCM. encrypt=1: in=plaintext -> out=cipher, mic[M] produced.
 * encrypt=0: in=cipher -> out=plaintext; if expected_mic is set, check it
 * (return 0 ok, -1 MIC error). CCMP: nlen=13, M=8, L=2. */
int rtl_aes_ccm(int encrypt, const uint8_t key[16],
                const uint8_t *nonce, int nlen,
                const uint8_t *aad, int alen,
                const uint8_t *in, int inlen,
                uint8_t *out, uint8_t *mic, int M,
                const uint8_t *expected_mic);

/* RFC3394 AES-Key-Unwrap (GTK from EAPOL msg3 using KEK). 0 ok, -1 error. */
int rtl_aes_unwrap(const uint8_t *kek, const uint8_t *wrapped, int wlen, uint8_t *out);

/* Decrypt a CCMP frame: complete 802.11 frame (header + CCMP header
 * + cipher + MIC) with a 16-byte key (TK unicast / GTK broadcast). Plaintext
 * (LLC/SNAP + payload) into out, length into *outlen. 0 ok, -1 MIC error.
 * AAD/Nonce per wpa_supplicant ccmp_aad_nonce. */
int rtl_ccmp_decrypt_frame(const uint8_t key[16], const uint8_t *frame, int len,
                           uint8_t *out, int *outlen);

/* Build a CCMP frame: 802.11 header (hdr, hdrlen) + payload -> out (complete
 * encrypted frame), *outlen set. pn = 48-bit Packet Number (incremented by the
 * caller). 0 ok. */
int rtl_ccmp_encrypt_frame(const uint8_t key[16], const uint8_t *hdr, int hdrlen,
                           const uint8_t *payload, int plen, uint64_t pn,
                           uint8_t *out, int *outlen);

/* CCM core self-test against NIST SP 800-38C. 0 = PASS. */
int rtl_ccmp_selftest(void);

#endif /* RTL_CCMP_H */
