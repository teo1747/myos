/* user/lib/tls/prf12.c -- see prf12.h. */
#include <string.h>
#include "prf12.h"
#include "crypto/hmac.h"
#include "crypto/sha256.h"

#define HLEN 32

void tls12_prf(const uint8_t *secret, size_t secret_len,
               const char *label,
               const uint8_t *seed, size_t seed_len,
               uint8_t *out, size_t out_len) {
    size_t llen = strlen(label);
    /* The label and seed are concatenated once and reused; A(i) is prepended
     * to that same buffer for each block. Bounded because every caller's seed
     * is two 32-byte randoms or one 32-byte hash. */
    uint8_t ls[64 + 96];
    if (llen + seed_len > sizeof ls) return;          /* refuse rather than truncate */
    memcpy(ls, label, llen);
    memcpy(ls + llen, seed, seed_len);
    size_t lslen = llen + seed_len;

    uint8_t a[HLEN];
    hmac_sha256(secret, secret_len, ls, lslen, a);     /* A(1) = HMAC(secret, seed) */

    uint8_t block[HLEN + sizeof ls];
    size_t done = 0;
    while (done < out_len) {
        memcpy(block, a, HLEN);
        memcpy(block + HLEN, ls, lslen);
        uint8_t h[HLEN];
        hmac_sha256(secret, secret_len, block, HLEN + lslen, h);
        size_t n = out_len - done;
        if (n > HLEN) n = HLEN;
        memcpy(out + done, h, n);
        done += n;
        hmac_sha256(secret, secret_len, a, HLEN, a);   /* A(i+1) = HMAC(secret, A(i)) */
    }
}

void tls12_master_secret(const uint8_t *pre_master, size_t pre_master_len,
                         const uint8_t client_random[32],
                         const uint8_t server_random[32],
                         uint8_t master[48]) {
    uint8_t seed[64];
    memcpy(seed, client_random, 32);
    memcpy(seed + 32, server_random, 32);
    tls12_prf(pre_master, pre_master_len, "master secret", seed, sizeof seed, master, 48);
}

void tls12_key_block(const uint8_t master[48],
                     const uint8_t client_random[32],
                     const uint8_t server_random[32],
                     uint8_t *out, size_t out_len) {
    /* SERVER random first. The two derivations use opposite orders, and
     * swapping them yields keys that are wrong in a way that only shows up as
     * a decryption failure several messages later. */
    uint8_t seed[64];
    memcpy(seed, server_random, 32);
    memcpy(seed + 32, client_random, 32);
    tls12_prf(master, 48, "key expansion", seed, sizeof seed, out, out_len);
}

void tls12_verify_data(const uint8_t master[48], const char *label,
                       const uint8_t transcript_hash[32], uint8_t out[12]) {
    tls12_prf(master, 48, label, transcript_hash, 32, out, 12);
}
