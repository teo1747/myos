#ifndef EMBK_TLS_TLS12_H
#define EMBK_TLS_TLS12_H
/* The TLS 1.2 half of the client (RFC 5246, RFC 4492, RFC 5288).
 *
 * 1.3 is not 1.2 with different numbers -- the two share the cipher and the
 * certificate format and essentially nothing else. Different key schedule
 * (PRF vs HKDF), different record framing (explicit nonce, different additional
 * data), different message flow, and a signature that covers the key exchange
 * parameters rather than the transcript. So it lives in its own file and
 * tls.c dispatches on what the server chose.
 *
 * WHY IT EXISTS AT ALL, given 1.3 is a decade old: a browser that speaks only
 * 1.3 cannot open xkcd.com. Most of the web has moved, and "most" is not the
 * same as "the site you wanted".
 *
 * Scope: ECDHE key exchange with an RSA or ECDSA server key, AES-128-GCM,
 * SHA-256. No static RSA (removed in 1.3 for good reasons and not worth
 * carrying), no CBC suites, no renegotiation, no session resumption. The
 * server certificate is verified exactly as it is for 1.3 -- same chain, same
 * anchors, same hostname rules -- plus the ServerKeyExchange signature, which
 * is what binds the ephemeral key to that certificate.
 */
#include <stdint.h>
#include <stddef.h>

struct tls_conn;

/* The 1.3 side's clock and socket helpers, shared rather than duplicated. */
void tls12_now_utc14(char out[15]);
int  tls12_io_send(struct tls_conn *c, const uint8_t *b, size_t n);
int  tls12_io_record(struct tls_conn *c);

/* Continue a handshake the server has answered with a 1.2 ServerHello.
 *
 * `sh` is that ServerHello handshake message (without the record header), and
 * the caller has already put the ClientHello and it into the transcript.
 * `priv`/`pub` are our X25519 pair, `crand` the client random.
 *
 * Returns 0, or a negative error in the same space tls_connect uses: -1
 * protocol/IO, -2 unsupported parameters, -100+X509_ERR_* for a certificate
 * that does not verify. */
/* The 1.2 record path, for tls_read/tls_write. The keys live in tls12.c --
 * one connection at a time, like the rest of this client. */
int tls12_seal(struct tls_conn *c, unsigned char type,
               const unsigned char *content, size_t len,
               unsigned char *out, size_t out_cap);
int tls12_open(struct tls_conn *c, const unsigned char *rec, size_t rec_len,
               unsigned char *out, size_t out_cap, unsigned char *out_type);

int tls12_client_handshake(struct tls_conn *c,
                           const uint8_t *sh, size_t sh_len,
                           const uint8_t crand[32],
                           const uint8_t priv[32], const uint8_t pub[32],
                           const char *server_name);

#endif /* EMBK_TLS_TLS12_H */
