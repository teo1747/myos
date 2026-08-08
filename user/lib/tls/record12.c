/* user/lib/tls/record12.c -- see record12.h. */
#include <string.h>
#include "record12.h"

void tls12_keys_init(struct tls12_keys *k, const uint8_t key[16], const uint8_t salt[4]) {
    aes128_gcm_init(&k->aead, key);
    memcpy(k->salt, salt, 4);
    k->seq = 0;
}

static void be64(uint8_t out[8], uint64_t v) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* seq(8) || type(1) || version(2) || plaintext_length(2) -- RFC 5246 §6.2.3.3. */
static void aad_of(uint8_t aad[13], uint64_t seq, uint8_t type, size_t plain_len) {
    be64(aad, seq);
    aad[8]  = type;
    aad[9]  = 0x03; aad[10] = 0x03;                 /* TLS 1.2 */
    aad[11] = (uint8_t)(plain_len >> 8);
    aad[12] = (uint8_t)plain_len;
}

int tls12_record_seal(struct tls12_keys *k, uint8_t type,
                      const uint8_t *content, size_t content_len,
                      uint8_t *out, size_t out_cap) {
    size_t body = TLS12_EXPLICIT_NONCE_LEN + content_len + TLS_TAG_LEN;
    if (content_len > TLS_RECORD_MAX_PLAINTEXT) return -1;
    if (out_cap < TLS_RECORD_HEADER_LEN + body) return -1;

    out[0] = type; out[1] = 0x03; out[2] = 0x03;
    out[3] = (uint8_t)(body >> 8); out[4] = (uint8_t)body;

    uint8_t *expl = out + TLS_RECORD_HEADER_LEN;
    be64(expl, k->seq);                              /* explicit nonce = seq */

    uint8_t nonce[12];
    memcpy(nonce, k->salt, 4);
    memcpy(nonce + 4, expl, 8);

    uint8_t aad[13];
    aad_of(aad, k->seq, type, content_len);

    uint8_t *ct = expl + TLS12_EXPLICIT_NONCE_LEN;
    aes128_gcm_seal(&k->aead, nonce, aad, sizeof aad, content, content_len,
                    ct, ct + content_len);
    k->seq++;
    return (int)(TLS_RECORD_HEADER_LEN + body);
}

int tls12_record_open(struct tls12_keys *k, const uint8_t *rec, size_t rec_len,
                      uint8_t *out, size_t out_cap, uint8_t *out_type) {
    if (rec_len < TLS_RECORD_HEADER_LEN + TLS12_EXPLICIT_NONCE_LEN + TLS_TAG_LEN) return -1;
    size_t body = ((size_t)rec[3] << 8) | rec[4];
    if (body + TLS_RECORD_HEADER_LEN != rec_len) return -1;

    size_t ct_len = body - TLS12_EXPLICIT_NONCE_LEN - TLS_TAG_LEN;
    if (ct_len > out_cap) return -1;

    const uint8_t *expl = rec + TLS_RECORD_HEADER_LEN;
    const uint8_t *ct   = expl + TLS12_EXPLICIT_NONCE_LEN;

    uint8_t nonce[12];
    memcpy(nonce, k->salt, 4);
    memcpy(nonce + 4, expl, 8);                      /* the SENDER's, not ours */

    uint8_t aad[13];
    aad_of(aad, k->seq, rec[0], ct_len);

    if (aes128_gcm_open(&k->aead, nonce, aad, sizeof aad, ct, ct_len,
                        ct + ct_len, out) != 0) {
        memset(out, 0, ct_len);                      /* nothing on failure */
        return -1;
    }
    k->seq++;
    if (out_type) *out_type = rec[0];                /* the header's, not a trailing byte */
    return (int)ct_len;
}
