#ifndef EMBK_TLS_RECORD12_H
#define EMBK_TLS_RECORD12_H
/* TLS 1.2 AEAD record layer for the AES-GCM suites (RFC 5288).
 *
 * Its own file because it shares nothing with 1.3's beyond the cipher. Three
 * things differ, and each is a silent decryption failure if got wrong:
 *
 *   THE NONCE is salt(4) || explicit(8) rather than iv XOR seq. The salt comes
 *   from the key block; the explicit half travels ON THE WIRE ahead of the
 *   ciphertext, which is why a 1.2 record is 8 bytes longer than its content
 *   plus tag. This client sends the sequence number as the explicit part,
 *   which is what everything else does and is safe because it never repeats
 *   under one key.
 *
 *   THE ADDITIONAL DATA is seq(8) || type(1) || version(2) || length(2), where
 *   the length is of the PLAINTEXT. 1.3 uses the 5-byte record header instead.
 *
 *   THE CONTENT TYPE is the record header's, not a trailing byte inside the
 *   plaintext. 1.2 has nothing to hide it behind.
 */
#include <stdint.h>
#include <stddef.h>
#include "crypto/gcm.h"
#include "record.h"     /* TLS_CT_*, TLS_RECORD_HEADER_LEN, TLS_TAG_LEN */

#define TLS12_EXPLICIT_NONCE_LEN 8

struct tls12_keys {
    struct aes128_gcm aead;
    uint8_t  salt[4];        /* the implicit half of the nonce */
    uint64_t seq;
};

void tls12_keys_init(struct tls12_keys *k, const uint8_t key[16], const uint8_t salt[4]);

/* Seal `content` of `type` into a record at `out`. Returns the whole record
 * length, or -1 if it will not fit. Advances the sequence number. */
int tls12_record_seal(struct tls12_keys *k, uint8_t type,
                      const uint8_t *content, size_t content_len,
                      uint8_t *out, size_t out_cap);

/* Open a complete record (header + explicit nonce + ciphertext + tag). Writes
 * the plaintext to `out`, sets *out_type from the RECORD HEADER, returns the
 * plaintext length. -1 on a bad tag or a malformed record, writing nothing. */
int tls12_record_open(struct tls12_keys *k, const uint8_t *rec, size_t rec_len,
                      uint8_t *out, size_t out_cap, uint8_t *out_type);

#endif /* EMBK_TLS_RECORD12_H */
