#ifndef EMBK_TLS_PRF12_H
#define EMBK_TLS_PRF12_H
/* The TLS 1.2 pseudo-random function (RFC 5246 §5).
 *
 * 1.3 replaced this entirely with HKDF, so the two versions share no key
 * schedule at all -- which is why this is its own file rather than a branch
 * inside keysched.c. Everything 1.2 derives comes through here: the master
 * secret from the ECDH shared secret, the key block from the master secret,
 * and the Finished message's verify_data from the handshake transcript.
 *
 *   PRF(secret, label, seed) = P_SHA256(secret, label || seed)
 *   P_hash(secret, seed)     = HMAC(secret, A(1) || seed) ||
 *                              HMAC(secret, A(2) || seed) || ...
 *   A(0) = seed,  A(i) = HMAC(secret, A(i-1))
 *
 * SHA-256 only. Every suite this client offers for 1.2 is a SHA-256 suite;
 * a SHA-384 one would need the same shape over a different hash, which is a
 * parameter this does not yet take.
 */
#include <stdint.h>
#include <stddef.h>

void tls12_prf(const uint8_t *secret, size_t secret_len,
               const char *label,
               const uint8_t *seed, size_t seed_len,
               uint8_t *out, size_t out_len);

/* master_secret = PRF(pre_master, "master secret", client_random || server_random)
 * -- 48 bytes, RFC 5246 §8.1. */
void tls12_master_secret(const uint8_t *pre_master, size_t pre_master_len,
                         const uint8_t client_random[32],
                         const uint8_t server_random[32],
                         uint8_t master[48]);

/* key_block = PRF(master, "key expansion", server_random || client_random).
 * Note the order: server first here, client first for the master secret. For
 * an AEAD suite the block is client_key || server_key || client_iv ||
 * server_iv, with no MAC keys -- the AEAD is the MAC. */
void tls12_key_block(const uint8_t master[48],
                     const uint8_t client_random[32],
                     const uint8_t server_random[32],
                     uint8_t *out, size_t out_len);

/* verify_data = PRF(master, label, Hash(handshake_messages))[0..11]. `label`
 * is "client finished" or "server finished". Twelve bytes for every suite
 * this client offers. */
void tls12_verify_data(const uint8_t master[48], const char *label,
                       const uint8_t transcript_hash[32], uint8_t out[12]);

#endif /* EMBK_TLS_PRF12_H */
