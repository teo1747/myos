#ifndef EMBK_TLS_HANDSHAKE_H
#define EMBK_TLS_HANDSHAKE_H
/* TLS 1.3 handshake messages (RFC 8446 §4): building the ClientHello, parsing
 * the ServerHello, the running Transcript-Hash, and the Finished MAC. This is
 * the wire-format layer; the state machine that sequences these lives in tls.c. */
#include <stdint.h>
#include <stddef.h>
#include "crypto/sha256.h"

/* Wire constants. */
#define TLS_VERSION_13        0x0304
#define TLS_VERSION_LEGACY    0x0303
#define TLS_SUITE_AES128_GCM  0x1301   /* TLS_AES_128_GCM_SHA256 (the 1.3 suite) */
#define TLS_VERSION_12        0x0303
/* tls_parse_server_hello: the server chose 1.2, not an error. */
#define TLS_SH_IS_12          (-3)
/* The 1.2 suites, both ECDHE + AES-128-GCM + SHA-256 (RFC 5289). */
#define TLS_SUITE12_ECDHE_ECDSA_AES128_GCM 0xC02B
#define TLS_SUITE12_ECDHE_RSA_AES128_GCM   0xC02F
#define TLS_GROUP_X25519      0x001d

/* HandshakeType (RFC 8446 §4). */
#define TLS_HS_CLIENT_HELLO         1
#define TLS_HS_SERVER_HELLO         2
#define TLS_HS_NEW_SESSION_TICKET   4
#define TLS_HS_ENCRYPTED_EXTENSIONS 8
#define TLS_HS_CERTIFICATE          11
#define TLS_HS_CERTIFICATE_VERIFY   15
#define TLS_HS_FINISHED             20

/* Transcript-Hash: a running SHA-256 over the concatenated handshake messages
 * (each message = type||uint24 len||body). Snapshots are non-destructive so the
 * schedule can hash the transcript at several points. */
struct tls_transcript { struct sha256_ctx ctx; };
void tls_transcript_init(struct tls_transcript *t);
void tls_transcript_update(struct tls_transcript *t, const uint8_t *msg, size_t len);
void tls_transcript_hash(const struct tls_transcript *t, uint8_t out[32]);

/* Build a ClientHello handshake message (type||len||body) into `out`. Offers
 * exactly TLS 1.3, TLS_AES_128_GCM_SHA256, x25519 (with `x25519_pub` as the
 * key_share), a standard signature_algorithms set, and SNI = `server_name`.
 * Returns the message length, or -1 if `cap` is too small. */
int tls_build_client_hello(uint8_t *out, size_t cap,
                           const uint8_t client_random[32],
                           const uint8_t session_id[32],
                           const uint8_t x25519_pub[32],
                           const char *server_name);

/* Parse a full ServerHello handshake message (type||len||body). On success
 * copies the server's x25519 public key_share to `server_pub` and returns 0.
 * Returns -1 malformed / wrong suite / no x25519 share, or -2 HelloRetryRequest
 * (unsupported in v1). */
int tls_parse_server_hello(const uint8_t *msg, size_t len, uint8_t server_pub[32]);

/* Finished verify_data = HMAC(HKDF-Expand-Label(base_secret,"finished",""),
 * transcript_hash) -- RFC 8446 §4.4.4. */
void tls_finished_mac(const uint8_t base_secret[32], const uint8_t transcript_hash[32],
                      uint8_t out[32]);

#endif /* EMBK_TLS_HANDSHAKE_H */
