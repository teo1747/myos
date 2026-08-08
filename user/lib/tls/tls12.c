/* user/lib/tls/tls12.c -- see tls12.h. */
#include <string.h>
#include "tls.h"
#include "tls12.h"
#include "prf12.h"
#include "record12.h"
#include "handshake.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "crypto/x25519.h"
#include "crypto/ecdsa.h"
#include "crypto/rsa.h"
#include "x509/cert.h"

/* Handshake message types we care about (RFC 5246 §7.4). */
#define HS_SERVER_HELLO         2
#define HS_CERTIFICATE          11
#define HS_SERVER_KEY_EXCHANGE  12
#define HS_SERVER_HELLO_DONE    14
#define HS_CLIENT_KEY_EXCHANGE  16
#define HS_FINISHED             20

/* The suites we offer for 1.2, both ECDHE + AES-128-GCM + SHA-256. Which one
 * the server picks tells us only how the ServerKeyExchange is SIGNED; the key
 * exchange and the record layer are identical either way. */
#define SUITE_ECDHE_RSA_AES128_GCM    0xC02F
#define SUITE_ECDHE_ECDSA_AES128_GCM  0xC02B

/* --- small readers, bounds-checked like handshake.c's ---------------------- */
struct rd { const uint8_t *p; size_t len, pos; int err; };
static uint8_t  g8(struct rd *b)  { if (b->pos + 1 > b->len) { b->err = 1; return 0; } return b->p[b->pos++]; }
static uint16_t g16(struct rd *b) { uint16_t h = g8(b); return (uint16_t)((h << 8) | g8(b)); }
static const uint8_t *gskip(struct rd *b, size_t n) {
    if (b->pos + n > b->len) { b->err = 1; return 0; }
    const uint8_t *p = b->p + b->pos; b->pos += n; return p;
}

/* Whole handshake messages are reassembled here: a server can split any of
 * them across records, and Certificate routinely is. */
struct flight {
    struct tls_conn *c;
    size_t off;          /* consumed bytes of c->hbuf */
};

static int flight_fill(struct flight *f) {
    struct tls_conn *c = f->c;
    int rl = tls12_io_record(c);
    if (rl < 0) return -1;
    if (c->recbuf[0] == TLS_CT_ALERT) return -1;
    if (c->recbuf[0] != TLS_CT_HANDSHAKE) return -1;
    size_t n = (size_t)rl - 5;
    if (c->hlen + n > sizeof c->hbuf) return -1;
    memcpy(c->hbuf + c->hlen, c->recbuf + 5, n);
    c->hlen += n;
    return 0;
}

/* Next complete handshake message: type, body, and the whole message (with its
 * 4-byte header) for the transcript. */
static int flight_next(struct flight *f, uint8_t *type,
                       const uint8_t **body, size_t *blen,
                       const uint8_t **whole, size_t *wlen) {
    struct tls_conn *c = f->c;
    for (;;) {
        size_t avail = c->hlen - f->off;
        if (avail >= 4) {
            const uint8_t *m = c->hbuf + f->off;
            size_t n = ((size_t)m[1] << 16) | ((size_t)m[2] << 8) | m[3];
            if (avail >= 4 + n) {
                *type = m[0]; *body = m + 4; *blen = n;
                *whole = m; *wlen = 4 + n;
                f->off += 4 + n;
                return 0;
            }
        }
        if (flight_fill(f)) return -1;
    }
}

/* --- ServerKeyExchange ----------------------------------------------------- *
 *
 * ECDHE params, then a signature over
 *   client_random || server_random || ServerKeyExchange.params
 * (RFC 4492 §5.4). That signature is the ONLY thing binding the ephemeral key
 * to the certificate: without checking it, an attacker who can speak to the
 * client substitutes their own key and the certificate is decoration. 1.3
 * folds the same job into CertificateVerify over the transcript.
 */
static int verify_ske(const struct x509_cert *leaf,
                      const uint8_t crand[32], const uint8_t srand[32],
                      const uint8_t *params, size_t params_len,
                      uint16_t scheme, const uint8_t *sig, size_t sig_len) {
    /* The signed content, hashed with whatever the scheme names. */
    uint8_t buf[64 + 512];
    if (params_len > sizeof buf - 64) return 0;
    memcpy(buf, crand, 32);
    memcpy(buf + 32, srand, 32);
    memcpy(buf + 64, params, params_len);
    size_t n = 64 + params_len;

    uint8_t dg[64]; size_t dglen = 32;
    unsigned hashid = scheme & 0xFF;          /* legacy (hash,sig) pair, low byte = sig */
    unsigned hash_of = (scheme >> 8) & 0xFF;  /* ...high byte = hash */
    if (hash_of == 4)      { sha256(buf, n, dg); dglen = 32; }
    else if (hash_of == 5) { sha384(buf, n, dg); dglen = 48; }
    else if (hash_of == 6) { sha512(buf, n, dg); dglen = 64; }
    else if (scheme == 0x0804 || scheme == 0x0805 || scheme == 0x0806) {
        /* RSA-PSS schemes name their hash in the low byte instead. */
        if (scheme == 0x0804)      { sha256(buf, n, dg); dglen = 32; }
        else if (scheme == 0x0805) { sha384(buf, n, dg); dglen = 48; }
        else                       { sha512(buf, n, dg); dglen = 64; }
    } else return 0;                           /* an unknown scheme is not a pass */

    if (leaf->key_type == X509_KEY_EC) {
        /* DER SEQUENCE { r INTEGER, s INTEGER } */
        struct rd b = { sig, sig_len, 0, 0 };
        if (g8(&b) != 0x30) return 0;
        size_t seqlen = g8(&b);
        if (seqlen & 0x80) { size_t k = seqlen & 0x7F; seqlen = 0;
                             for (size_t i = 0; i < k; i++) seqlen = (seqlen << 8) | g8(&b); }
        if (g8(&b) != 0x02) return 0;
        size_t rlen = g8(&b); const uint8_t *r = gskip(&b, rlen);
        if (g8(&b) != 0x02) return 0;
        size_t slen = g8(&b); const uint8_t *s = gskip(&b, slen);
        if (b.err || !r || !s) return 0;
        while (rlen && *r == 0) { r++; rlen--; }
        while (slen && *s == 0) { s++; slen--; }
        const struct ec_curve *cv = leaf->curve == X509_CURVE_P384 ? ec_p384() : ec_p256();
        return ecdsa_verify(cv, leaf->qx, leaf->qy, dg, dglen, r, rlen, s, slen);
    }
    if (leaf->key_type == X509_KEY_RSA) {
        if (scheme == 0x0804 || scheme == 0x0805 || scheme == 0x0806) {
            rsa_hash_fn h = scheme == 0x0804 ? (rsa_hash_fn)sha256
                          : scheme == 0x0805 ? (rsa_hash_fn)sha384 : (rsa_hash_fn)sha512;
            return rsa_pss_verify(leaf->rsa_n, leaf->rsa_n_len, leaf->rsa_e, leaf->rsa_e_len,
                                  h, dglen, dg, sig, sig_len);
        }
        int alg = dglen == 32 ? RSA_HASH_SHA256 : dglen == 48 ? RSA_HASH_SHA384 : RSA_HASH_SHA512;
        return rsa_pkcs1_verify(leaf->rsa_n, leaf->rsa_n_len, leaf->rsa_e, leaf->rsa_e_len,
                                alg, dg, dglen, sig, sig_len);
    }
    (void)hashid;
    return 0;
}

/* The 1.2 AEAD state. One connection at a time -- the same assumption the rest
 * of this client makes -- and here rather than in struct tls_conn so that the
 * 1.2 record layer's types stay out of the header every TLS caller includes. */
static struct tls12_keys g_rx, g_tx;

int tls12_seal(struct tls_conn *c, unsigned char type,
               const unsigned char *content, size_t len,
               unsigned char *out, size_t out_cap) {
    (void)c; return tls12_record_seal(&g_tx, type, content, len, out, out_cap);
}
int tls12_open(struct tls_conn *c, const unsigned char *rec, size_t rec_len,
               unsigned char *out, size_t out_cap, unsigned char *out_type) {
    (void)c; return tls12_record_open(&g_rx, rec, rec_len, out, out_cap, out_type);
}

/* --- the handshake --------------------------------------------------------- */
int tls12_client_handshake(struct tls_conn *c,
                           const uint8_t *sh, size_t sh_len,
                           const uint8_t crand[32],
                           const uint8_t priv[32], const uint8_t pub[32],
                           const char *server_name) {
    /* ServerHello: version(2) random(32) session_id<0..32> suite(2) comp(1) */
    struct rd b = { sh + 4, sh_len - 4, 0, 0 };     /* past the 4-byte hs header */
    g16(&b);
    const uint8_t *srand_p = gskip(&b, 32);
    if (!srand_p) return -1;
    uint8_t srand[32]; memcpy(srand, srand_p, 32);
    uint8_t sidlen = g8(&b); gskip(&b, sidlen);
    uint16_t suite = g16(&b);
    if (b.err) return -1;
    if (suite != SUITE_ECDHE_RSA_AES128_GCM && suite != SUITE_ECDHE_ECDSA_AES128_GCM)
        return -2;                                   /* we offered two; it picked neither */

    struct flight f = { c, 0 };
    c->hlen = 0;

    /* Certificate, ServerKeyExchange, ServerHelloDone -- in that order, though
     * a server may interleave a CertificateRequest we decline to handle. */
    struct x509_cert leaf;
    int have_leaf = 0;
    uint8_t skx_params[512]; size_t skx_params_len = 0;
    uint16_t skx_scheme = 0;
    uint8_t skx_sig[512]; size_t skx_sig_len = 0;
    uint8_t server_pub[32]; int have_pub = 0;

    for (;;) {
        uint8_t t; const uint8_t *body, *whole; size_t blen, wlen;
        if (flight_next(&f, &t, &body, &blen, &whole, &wlen)) return -1;
        tls_transcript_update(&c->tr, whole, wlen);

        if (t == HS_CERTIFICATE) {
            /* 1.2's Certificate has no per-cert extensions and no context. */
            if (blen < 3) return -1;
            size_t list = ((size_t)body[0] << 16) | ((size_t)body[1] << 8) | body[2];
            const uint8_t *p = body + 3, *end = body + 3 + list;
            if (end > body + blen) return -1;
            struct x509_cert certs[6]; int nc = 0;
            while (p < end && nc < 6) {
                if (p + 3 > end) return -1;
                size_t clen = ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2]; p += 3;
                if (p + clen > end) return -1;
                if (x509_parse(p, clen, &certs[nc]) != 0) return -100 + X509_ERR_SIG;
                p += clen; nc++;
            }
            if (nc == 0) return -1;
            char now[15]; tls12_now_utc14(now);
            int v = x509_verify_chain(certs, nc, server_name, now);
            if (v != X509_OK) return -100 + v;        /* same rules as 1.3 */
            leaf = certs[0]; have_leaf = 1;
            c->verified = 1;
        } else if (t == HS_SERVER_KEY_EXCHANGE) {
            /* curve_type(1)=named_curve(3), namedcurve(2), point<1..255>,
             * then scheme(2) and signature<0..2^16>. */
            struct rd s = { body, blen, 0, 0 };
            if (g8(&s) != 3) return -2;               /* only named curves */
            uint16_t group = g16(&s);
            uint8_t plen = g8(&s);
            const uint8_t *point = gskip(&s, plen);
            if (s.err || !point) return -1;
            size_t params_len = s.pos;                /* everything signed so far */
            if (params_len > sizeof skx_params) return -1;
            memcpy(skx_params, body, params_len);
            skx_params_len = params_len;
            skx_scheme = g16(&s);
            uint16_t siglen = g16(&s);
            const uint8_t *sig = gskip(&s, siglen);
            if (s.err || !sig || siglen > sizeof skx_sig) return -1;
            memcpy(skx_sig, sig, siglen); skx_sig_len = siglen;

            if (group != TLS_GROUP_X25519 || plen != 32) return -2;
            memcpy(server_pub, point, 32); have_pub = 1;
        } else if (t == HS_SERVER_HELLO_DONE) {
            break;
        }
        /* anything else in the flight is ignored on purpose */
    }
    if (!have_leaf || !have_pub) return -1;

    if (!verify_ske(&leaf, crand, srand, skx_params, skx_params_len,
                    skx_scheme, skx_sig, skx_sig_len))
        return -100 + X509_ERR_SIG;   /* the key exchange is not the cert's */

    /* ClientKeyExchange: our public point. */
    uint8_t cke[4 + 1 + 32];
    cke[0] = HS_CLIENT_KEY_EXCHANGE; cke[1] = 0; cke[2] = 0; cke[3] = 33;
    cke[4] = 32; memcpy(cke + 5, pub, 32);
    tls_transcript_update(&c->tr, cke, sizeof cke);

    uint8_t rec[512];
    rec[0] = TLS_CT_HANDSHAKE; rec[1] = 3; rec[2] = 3;
    rec[3] = 0; rec[4] = (uint8_t)sizeof cke;
    memcpy(rec + 5, cke, sizeof cke);
    if (tls12_io_send(c, rec, 5 + sizeof cke)) return -1;

    /* The secrets. */
    uint8_t shared[32];
    x25519(shared, priv, server_pub);
    uint8_t master[48];
    tls12_master_secret(shared, 32, crand, srand, master);

    /* key_block for an AEAD suite: no MAC keys, 16-byte keys, 4-byte salts. */
    uint8_t kb[16 + 16 + 4 + 4];
    tls12_key_block(master, crand, srand, kb, sizeof kb);
    tls12_keys_init(&g_tx, kb, kb + 32);
    tls12_keys_init(&g_rx, kb + 16, kb + 36);

    /* ChangeCipherSpec -- a record, not a handshake message, so it is NOT in
     * the transcript. Getting that wrong breaks Finished and nothing else. */
    uint8_t ccs[6] = { TLS_CT_CHANGE_CIPHER_SPEC, 3, 3, 0, 1, 1 };
    if (tls12_io_send(c, ccs, sizeof ccs)) return -1;

    /* Finished, encrypted under the new keys. */
    uint8_t th[32]; tls_transcript_hash(&c->tr, th);
    uint8_t fin[4 + 12];
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    tls12_verify_data(master, "client finished", th, fin + 4);
    tls_transcript_update(&c->tr, fin, sizeof fin);
    int n = tls12_record_seal(&g_tx, TLS_CT_HANDSHAKE, fin, sizeof fin, rec, sizeof rec);
    if (n < 0 || tls12_io_send(c, rec, (size_t)n)) return -1;

    /* The server's verify_data covers the transcript INCLUDING our Finished,
     * so it needs a fresh hash -- reusing the one our own Finished was built
     * from makes every handshake fail at the last message with nothing else
     * wrong. */
    uint8_t th_after[32]; tls_transcript_hash(&c->tr, th_after);
    uint8_t sfin_expect[12];
    tls12_verify_data(master, "server finished", th_after, sfin_expect);

    int saw_ccs = 0;
    for (;;) {
        int rl = tls12_io_record(c);
        if (rl < 0) return -1;
        if (c->recbuf[0] == TLS_CT_CHANGE_CIPHER_SPEC) { saw_ccs = 1; continue; }
        if (c->recbuf[0] == TLS_CT_ALERT) return -1;
        if (!saw_ccs) return -1;                     /* Finished must be protected */
        uint8_t itype;
        uint8_t plain[256];
        int m = tls12_record_open(&g_rx, c->recbuf, (size_t)rl, plain, sizeof plain, &itype);
        if (m < 0) return -3;
        if (itype != TLS_CT_HANDSHAKE || m < 16 || plain[0] != HS_FINISHED) return -1;
        /* Constant-time enough: a mismatch is fatal either way. */
        int diff = 0;
        for (int i = 0; i < 12; i++) diff |= plain[4 + i] ^ sfin_expect[i];
        if (diff) return -3;                         /* the server proved nothing */
        break;
    }

    c->version = 12;
    c->established = 1;
    return 0;
}
