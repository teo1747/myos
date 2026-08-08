/* TLS 1.3 handshake message wire format (RFC 8446 §4). */
#include "handshake.h"
#include "keysched.h"
#include "crypto/hmac.h"
#include <string.h>

/* ---- Transcript-Hash ---------------------------------------------------- */

void tls_transcript_init(struct tls_transcript *t) { sha256_init(&t->ctx); }
void tls_transcript_update(struct tls_transcript *t, const uint8_t *msg, size_t len) {
    sha256_update(&t->ctx, msg, len);
}
void tls_transcript_hash(const struct tls_transcript *t, uint8_t out[32]) {
    struct sha256_ctx snap = t->ctx;          /* copy: snapshot without finalizing */
    sha256_final(&snap, out);
}

/* ---- a tiny length-backpatching writer ---------------------------------- */

struct wbuf { uint8_t *p; size_t cap, len; int err; };

static void w8(struct wbuf *b, uint8_t v) {
    if (b->len + 1 > b->cap) { b->err = 1; return; }
    b->p[b->len++] = v;
}
static void w16(struct wbuf *b, uint16_t v) { w8(b, (uint8_t)(v >> 8)); w8(b, (uint8_t)v); }
static void wbytes(struct wbuf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) { b->err = 1; return; }
    memcpy(b->p + b->len, d, n); b->len += n;
}
/* Reserve a 2- or 3-byte length field; fill it in later with the bytes written
 * since. */
static size_t open16(struct wbuf *b) { size_t o = b->len; w16(b, 0); return o; }
static void close16(struct wbuf *b, size_t o) {
    if (b->err) return;
    size_t n = b->len - (o + 2);
    b->p[o] = (uint8_t)(n >> 8); b->p[o + 1] = (uint8_t)n;
}
static size_t open24(struct wbuf *b) { size_t o = b->len; w8(b, 0); w8(b, 0); w8(b, 0); return o; }
static void close24(struct wbuf *b, size_t o) {
    if (b->err) return;
    size_t n = b->len - (o + 3);
    b->p[o] = (uint8_t)(n >> 16); b->p[o + 1] = (uint8_t)(n >> 8); b->p[o + 2] = (uint8_t)n;
}

int tls_build_client_hello(uint8_t *out, size_t cap,
                           const uint8_t client_random[32],
                           const uint8_t session_id[32],
                           const uint8_t x25519_pub[32],
                           const char *server_name) {
    struct wbuf b = { out, cap, 0, 0 };

    w8(&b, TLS_HS_CLIENT_HELLO);
    size_t hs = open24(&b);
        w16(&b, TLS_VERSION_LEGACY);            /* legacy_version */
        wbytes(&b, client_random, 32);
        w8(&b, 32); wbytes(&b, session_id, 32); /* legacy_session_id (middlebox compat) */
        size_t cs = open16(&b);                 /* cipher_suites */
            w16(&b, TLS_SUITE_AES128_GCM);          /* 1.3 */
            /* ...and the 1.2 suites, because a server that speaks only 1.2 is
             * still a server someone wants to read. Both are ECDHE + AES-128-GCM
             * + SHA-256; which one is chosen decides only how the
             * ServerKeyExchange is signed. */
            w16(&b, TLS_SUITE12_ECDHE_ECDSA_AES128_GCM);
            w16(&b, TLS_SUITE12_ECDHE_RSA_AES128_GCM);
        close16(&b, cs);
        w8(&b, 1); w8(&b, 0);                   /* legacy_compression_methods: null */
        size_t ext = open16(&b);                /* extensions */
            /* supported_versions (43) = [0x0304, 0x0303], best first. A server
             * that understands the extension picks 1.3; one that does not
             * ignores it and answers 1.2 from legacy_version, which is exactly
             * how the two versions coexist on one ClientHello. */
            w16(&b, 43); { size_t e = open16(&b); w8(&b, 4);
                w16(&b, TLS_VERSION_13); w16(&b, TLS_VERSION_12); close16(&b, e); }
            /* ec_point_formats (11) = [uncompressed]. 1.3 dropped it; plenty of
             * 1.2 servers still refuse a ClientHello without it. */
            w16(&b, 11); { size_t e = open16(&b); w8(&b, 1); w8(&b, 0); close16(&b, e); }
            /* supported_groups (10) = [x25519] */
            w16(&b, 10); { size_t e = open16(&b); size_t l = open16(&b);
                w16(&b, TLS_GROUP_X25519); close16(&b, l); close16(&b, e); }
            /* signature_algorithms (13) -- required by servers even without verify */
            w16(&b, 13); { size_t e = open16(&b); size_t l = open16(&b);
                w16(&b, 0x0403);  /* ecdsa_secp256r1_sha256 */
                w16(&b, 0x0503);  /* ecdsa_secp384r1_sha384 -- github's chain uses it */
                w16(&b, 0x0804);  /* rsa_pss_rsae_sha256 */
                w16(&b, 0x0401);  /* rsa_pkcs1_sha256 */
                w16(&b, 0x0805);  /* rsa_pss_rsae_sha384 */
                w16(&b, 0x0806);  /* rsa_pss_rsae_sha512 */
                w16(&b, 0x0807);  /* ed25519 */
                close16(&b, l); close16(&b, e); }
            /* key_share (51) = [x25519: pub] */
            w16(&b, 51); { size_t e = open16(&b); size_t shares = open16(&b);
                w16(&b, TLS_GROUP_X25519); size_t k = open16(&b);
                wbytes(&b, x25519_pub, 32); close16(&b, k);
                close16(&b, shares); close16(&b, e); }
            /* server_name (0) -- SNI */
            if (server_name && *server_name) {
                size_t nlen = strlen(server_name);
                w16(&b, 0); { size_t e = open16(&b); size_t list = open16(&b);
                    w8(&b, 0);                   /* name_type = host_name */
                    size_t n = open16(&b); wbytes(&b, server_name, nlen); close16(&b, n);
                    close16(&b, list); close16(&b, e); }
            }
        close16(&b, ext);
    close24(&b, hs);

    return b.err ? -1 : (int)b.len;
}

/* ---- a matching bounds-checked reader ----------------------------------- */

struct rbuf { const uint8_t *p; size_t len, pos; int err; };
static uint8_t  r8(struct rbuf *b)  { if (b->pos + 1 > b->len) { b->err = 1; return 0; } return b->p[b->pos++]; }
static uint16_t r16(struct rbuf *b) { uint16_t h = r8(b); return (uint16_t)((h << 8) | r8(b)); }
static const uint8_t *rskip(struct rbuf *b, size_t n) {
    if (b->pos + n > b->len) { b->err = 1; return 0; }
    const uint8_t *p = b->p + b->pos; b->pos += n; return p;
}

int tls_parse_server_hello(const uint8_t *msg, size_t len, uint8_t server_pub[32]) {
    /* The HelloRetryRequest "random" sentinel (RFC 8446 §4.1.3). */
    static const uint8_t hrr[32] = {
        0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };

    struct rbuf b = { msg, len, 0, 0 };
    if (r8(&b) != TLS_HS_SERVER_HELLO) return -1;
    uint32_t blen = ((uint32_t)r8(&b) << 16) | ((uint32_t)r8(&b) << 8) | r8(&b);
    if (b.err || blen != b.len - b.pos) return -1;

    r16(&b);                                    /* legacy_version */
    const uint8_t *rand = rskip(&b, 32);
    if (b.err) return -1;
    if (memcmp(rand, hrr, 32) == 0) return -2;  /* HelloRetryRequest */

    uint8_t sid_len = r8(&b); rskip(&b, sid_len);      /* legacy_session_id_echo */
    uint16_t suite = r16(&b);
    r8(&b);                                     /* legacy_compression_method */
    if (b.err) return -1;
    /* A 1.2 SUITE IS THE ANSWER, not an error. A server that picked one has
     * told us the version before any extension does -- and a 1.2 ServerHello
     * may carry no extensions at all, so waiting to find (or not find)
     * supported_versions means failing on the suite check first. */
    if (suite == TLS_SUITE12_ECDHE_ECDSA_AES128_GCM ||
        suite == TLS_SUITE12_ECDHE_RSA_AES128_GCM) return TLS_SH_IS_12;
    if (suite != TLS_SUITE_AES128_GCM) return -1;

    uint16_t ext_total = r16(&b);
    size_t ext_end = b.pos + ext_total;
    if (ext_end > b.len) return -1;

    int have_share = 0, version_ok = 0;
    while (b.pos < ext_end && !b.err) {
        uint16_t etype = r16(&b);
        uint16_t elen  = r16(&b);
        size_t enext = b.pos + elen;
        if (enext > ext_end) return -1;
        if (etype == 43) {                      /* supported_versions: selected */
            if (r16(&b) == TLS_VERSION_13) version_ok = 1;
        } else if (etype == 51) {               /* key_share: server share */
            uint16_t group = r16(&b);
            uint16_t klen  = r16(&b);
            const uint8_t *key = rskip(&b, klen);
            if (!b.err && group == TLS_GROUP_X25519 && klen == 32) {
                memcpy(server_pub, key, 32); have_share = 1;
            }
        }
        b.pos = enext;                          /* skip any unconsumed ext bytes */
    }
    if (b.err) return -1;
    /* NO supported_versions extension means the server answered TLS 1.2, which
     * is not an error -- it is the other half of the client. Say so distinctly
     * so the caller can hand off rather than fail. */
    if (!version_ok) return TLS_SH_IS_12;
    if (!have_share) return -1;
    return 0;
}

void tls_finished_mac(const uint8_t base_secret[32], const uint8_t transcript_hash[32],
                      uint8_t out[32]) {
    uint8_t fkey[32];
    tls_expand_label(base_secret, "finished", NULL, 0, fkey, 32);
    hmac_sha256(fkey, 32, transcript_hash, 32, out);
}
