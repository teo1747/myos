/* libtls -- the TLS 1.3 client state machine (RFC 8446). Drives one full
 * handshake over a blocking TCP socket, then AEAD-protects application data.
 *
 * Handshake shape (1-RTT, RFC 8446 §2):
 *   -> ClientHello                                   (plaintext record)
 *   <- ServerHello                                   (plaintext record)
 *      == derive handshake traffic keys ==
 *   <- {EncryptedExtensions, Certificate*, CertificateVerify*, Finished}
 *                                                    (records under server hs key)
 *      == verify server Finished MAC ==   (* cert parsed, NOT verified: T2)
 *   -> {Finished}                                    (record under client hs key)
 *      == derive application traffic keys ==
 *   <> application data                              (records under app keys)
 */
#include "tls.h"
#include "tls12.h"
#include "keysched.h"
#include "cert.h"            /* x509 chain verification (T3) */
#include "asn1.h"
#include "x25519.h"
#include "ecdsa.h"
#include "rsa.h"
#include "sha512.h"
#include "crypto/sha256.h"
#include <string.h>
#include <unistd.h>
#include <time.h>

extern int getentropy(void *buf, size_t len);   /* RDRAND-backed (user/lib/syscalls.c) */

/* ---- blocking socket I/O ------------------------------------------------ */

static int io_send_all(int fd, const uint8_t *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        long w = write(fd, b + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}
static int io_recv_exact(int fd, uint8_t *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        long r = read(fd, b + off, n - off);
        if (r <= 0) return -1;           /* 0 = EOF, <0 = error */
        off += (size_t)r;
    }
    return 0;
}

/* Read exactly one TLS record into c->recbuf. Returns the total record length
 * (header + body), or -1. */
static int recv_record(struct tls_conn *c) {
    if (io_recv_exact(c->fd, c->recbuf, 5)) return -1;
    size_t blen = ((size_t)c->recbuf[3] << 8) | c->recbuf[4];
    if (5 + blen > sizeof c->recbuf) return -1;
    if (blen && io_recv_exact(c->fd, c->recbuf + 5, blen)) return -1;
    return (int)(5 + blen);
}

/* Send a TLSPlaintext record (used for the ClientHello, before keys exist). */
/* The 1.2 handshake lives in its own file and needs these two: one record in,
 * one buffer out. Exposed rather than duplicated -- the socket discipline
 * (short reads, exact lengths) is the same for both versions. */
int tls12_io_send(struct tls_conn *c, const uint8_t *b, size_t n) { return io_send_all(c->fd, b, n); }
int tls12_io_record(struct tls_conn *c) { return recv_record(c); }
void tls12_now_utc14(char out[15]);

static int send_plaintext(struct tls_conn *c, uint8_t type, const uint8_t *frag, size_t len) {
    uint8_t hdr[5] = { type, 0x03, 0x03, (uint8_t)(len >> 8), (uint8_t)len };
    if (io_send_all(c->fd, hdr, 5)) return -1;
    return io_send_all(c->fd, frag, len);
}

/* Install AEAD keys for one direction from a traffic secret. */
static void install_keys(struct tls_keys *k, const uint8_t secret[32]) {
    uint8_t key[16], iv[12];
    tls_expand_label(secret, "key", NULL, 0, key, 16);
    tls_expand_label(secret, "iv",  NULL, 0, iv, 12);
    tls_keys_init(k, key, iv);
}

/* ---- certificate verification (T3) -------------------------------------- */

/* Current time as "yyyymmddhhmmss" UTC, from the OS clock. */
/* Shared with the 1.2 path, which needs the same clock for the same reason. */
void tls12_now_utc14(char out[15]);
static void now_utc14(char out[15]) {
    time_t t = time(NULL);
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, 15, "%Y%m%d%H%M%S", &g);
}

/* Parse a DER ECDSA-Sig-Value SEQUENCE { r INTEGER, s INTEGER }, leading zeros
 * stripped. Returns 0 on success. */
static int parse_sig(const uint8_t *der, size_t len,
                     const uint8_t **r, size_t *rl, const uint8_t **s, size_t *sl) {
    struct der_tlv seq, ri, si;
    if (der_parse(der, der + len, &seq) || seq.tag != DER_SEQUENCE) return -1;
    if (der_parse(seq.val, der_end(&seq), &ri) || ri.tag != DER_INTEGER) return -1;
    if (der_parse(der_end(&ri), der_end(&seq), &si) || si.tag != DER_INTEGER) return -1;
    const uint8_t *rp = ri.val; size_t rn = ri.len;
    const uint8_t *sp = si.val; size_t sn = si.len;
    while (rn > 1 && rp[0] == 0) { rp++; rn--; }
    while (sn > 1 && sp[0] == 0) { sp++; sn--; }
    *r = rp; *rl = rn; *s = sp; *sl = sn;
    return 0;
}

/* Authenticate the peer: parse the Certificate chain, verify it to a trust
 * anchor with hostname + validity, then verify the CertificateVerify signature
 * with the leaf key. Returns 0 if authenticated, negative otherwise. */
static const char CV_LABEL[] = "TLS 1.3, server CertificateVerify";

static int tls_verify_peer(struct tls_conn *c, const char *host) {
    if (!host || c->certmsg_len < 8) return -30;

    /* Certificate message: header(4) | ctx<0..255> | cert_list<0..2^24>. */
    const uint8_t *p = c->certmsg + 4, *end = c->certmsg + c->certmsg_len;
    if (p >= end) return -30;
    uint8_t ctxlen = *p++; p += ctxlen;
    if (p + 3 > end) return -30;
    size_t list_len = ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2]; p += 3;
    const uint8_t *lend = p + list_len;
    if (lend > end) return -30;

    struct x509_cert certs[6]; int nc = 0;
    while (p < lend && nc < 6) {
        if (p + 3 > lend) return -30;
        size_t clen = ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2]; p += 3;
        if (p + clen > lend) return -30;
        const uint8_t *cder = p; p += clen;
        if (p + 2 > lend) return -30;
        size_t extlen = ((size_t)p[0] << 8) | p[1]; p += 2 + extlen;
        if (x509_parse(cder, clen, &certs[nc]) != 0) return -31;   /* cert #nc parse */
        nc++;
    }
    if (nc == 0) return -30;

    char now[15]; now_utc14(now);
    int v = x509_verify_chain(certs, nc, host, now);
    if (v != X509_OK) return v;                     /* untrusted / wrong host / expired */

    /* CertificateVerify: signature over 64*0x20 || label || 0x00 || Hash(CH..Cert). */
    uint8_t content[64 + sizeof CV_LABEL - 1 + 1 + 32];
    size_t n = 0;
    memset(content, 0x20, 64); n = 64;
    memcpy(content + n, CV_LABEL, sizeof CV_LABEL - 1); n += sizeof CV_LABEL - 1;
    content[n++] = 0x00;
    memcpy(content + n, c->th_cert, 32); n += 32;

    uint8_t dg[64];
    /* ECDSA schemes: hash the content, then ECDSA-verify with the leaf EC key. */
    if (c->cv_alg == 0x0403 || c->cv_alg == 0x0503) {
        const struct ec_curve *ec; size_t dl;
        if (c->cv_alg == 0x0403) { ec = ec_p256(); sha256(content, n, dg); dl = 32; }
        else                     { ec = ec_p384(); sha384(content, n, dg); dl = 48; }
        if (certs[0].key_type != X509_KEY_EC) return -20;
        const uint8_t *r, *s; size_t rl, sl;
        if (parse_sig(c->cv_sig, c->cv_sig_len, &r, &rl, &s, &sl)) return -21;
        if (!ecdsa_verify(ec, certs[0].qx, certs[0].qy, dg, dl, r, rl, s, sl)) return -22;
        return 0;
    }
    /* RSA-PSS schemes (TLS 1.3's RSA CertificateVerify): mHash = Hash(content),
     * then PSS-verify with the leaf RSA key. */
    if (c->cv_alg == 0x0804 || c->cv_alg == 0x0805 || c->cv_alg == 0x0806) {
        if (certs[0].key_type != X509_KEY_RSA) return -20;
        rsa_hash_fn hf; size_t hl;
        if (c->cv_alg == 0x0804)      { hf = sha256; hl = 32; }
        else if (c->cv_alg == 0x0805) { hf = sha384; hl = 48; }
        else                          { hf = sha512; hl = 64; }
        hf(content, n, dg);
        if (!rsa_pss_verify(certs[0].rsa_n, certs[0].rsa_n_len, certs[0].rsa_e, certs[0].rsa_e_len,
                            hf, hl, dg, c->cv_sig, c->cv_sig_len)) return -22;
        return 0;
    }
    return -20;                                     /* unsupported signature scheme */
}

/* ---- handshake ---------------------------------------------------------- */

int tls_connect(struct tls_conn *c, int fd, const char *server_name) {
    memset(c, 0, sizeof *c);
    c->fd = fd;

    uint8_t priv[32], pub[32], crand[32], sid[32];
    if (getentropy(priv, 32) || getentropy(crand, 32) || getentropy(sid, 32)) return -1;
    x25519_base(pub, priv);

    /* ClientHello (plaintext record); start the transcript. */
    uint8_t ch[1024];
    int chlen = tls_build_client_hello(ch, sizeof ch, crand, sid, pub, server_name);
    if (chlen < 0) return -1;
    tls_transcript_init(&c->tr);
    tls_transcript_update(&c->tr, ch, (size_t)chlen);
    if (send_plaintext(c, TLS_CT_HANDSHAKE, ch, (size_t)chlen)) return -1;

    /* ServerHello (plaintext handshake record; skip any middlebox CCS). */
    int rl;
    do {
        rl = recv_record(c);
        if (rl < 0) return -1;
    } while (c->recbuf[0] == TLS_CT_CHANGE_CIPHER_SPEC);
    if (c->recbuf[0] != TLS_CT_HANDSHAKE) return -1;

    uint8_t server_pub[32];
    size_t shlen = (size_t)rl - 5;
    int pr = tls_parse_server_hello(c->recbuf + 5, shlen, server_pub);
    if (pr == -2) return -2;              /* HelloRetryRequest unsupported */
    if (pr == TLS_SH_IS_12) {
        /* The server does not speak 1.3. Hand the rest of the handshake to the
         * 1.2 client -- same socket, same transcript, same certificate rules,
         * an entirely different key schedule and record layer. */
        tls_transcript_update(&c->tr, c->recbuf + 5, shlen);
        return tls12_client_handshake(c, c->recbuf + 5, shlen, crand, priv, pub, server_name);
    }
    if (pr) return -1;
    tls_transcript_update(&c->tr, c->recbuf + 5, shlen);
    c->version = 13;

    /* Key schedule up to the handshake traffic keys (RFC 8446 §7.1). */
    uint8_t shared[32], empty_hash[32], early[32], derived[32], th[32], key[16], iv[12];
    x25519(shared, priv, server_pub);
    sha256("", 0, empty_hash);
    tls_extract(NULL, NULL, early);
    tls_derive_secret(early, "derived", empty_hash, derived);
    tls_extract(derived, shared, c->hs_secret);        /* Handshake Secret */
    tls_transcript_hash(&c->tr, th);                   /* Hash(CH || SH) */
    tls_derive_secret(c->hs_secret, "c hs traffic", th, c->c_hs);
    tls_derive_secret(c->hs_secret, "s hs traffic", th, c->s_hs);
    install_keys(&c->rx, c->s_hs);                     /* server -> client */
    install_keys(&c->tx, c->c_hs);                     /* client -> server */
    (void)key; (void)iv;

    /* Server flight: EncryptedExtensions, Certificate*, CertificateVerify*,
     * Finished -- reassembled across records, then parsed message by message. */
    int server_fin = 0;
    while (!server_fin) {
        rl = recv_record(c);
        if (rl < 0) return -1;
        if (c->recbuf[0] == TLS_CT_CHANGE_CIPHER_SPEC) continue;
        if (c->recbuf[0] != TLS_CT_APPLICATION_DATA) return -1;

        uint8_t itype;
        int n = tls_record_open(&c->rx, c->recbuf, (size_t)rl,
                                c->hbuf + c->hlen, sizeof c->hbuf - c->hlen, &itype);
        if (n < 0) return -1;
        if (itype == TLS_CT_ALERT) return -1;
        if (itype != TLS_CT_HANDSHAKE) return -1;
        c->hlen += (size_t)n;

        size_t off = 0;
        while (c->hlen - off >= 4) {
            const uint8_t *m = c->hbuf + off;
            size_t mlen = ((size_t)m[1] << 16) | ((size_t)m[2] << 8) | m[3];
            if (c->hlen - off < 4 + mlen) break;       /* message spans more records */
            size_t total = 4 + mlen;

            if (m[0] == TLS_HS_FINISHED) {
                /* Verify the server Finished MAC over Hash(CH..CertificateVerify)
                 * BEFORE folding the Finished into the transcript. This is the
                 * milestone gate: it authenticates the whole handshake. */
                uint8_t want[32];
                tls_transcript_hash(&c->tr, th);
                tls_finished_mac(c->s_hs, th, want);
                if (mlen != 32 || memcmp(want, m + 4, 32) != 0) return -3;
                tls_transcript_update(&c->tr, m, total);
                server_fin = 1;
                off += total;
                break;
            }
            if (m[0] == TLS_HS_CERTIFICATE) {
                /* Stash the Certificate message (chain parsed after the flight),
                 * fold it in, then snapshot Hash(CH..Certificate) -- exactly what
                 * the CertificateVerify signs (RFC 8446 §4.4.3). */
                if (total <= sizeof c->certmsg) { memcpy(c->certmsg, m, total); c->certmsg_len = total; }
                tls_transcript_update(&c->tr, m, total);
                tls_transcript_hash(&c->tr, c->th_cert);
                off += total;
                continue;
            }
            if (m[0] == TLS_HS_CERTIFICATE_VERIFY && mlen >= 4) {
                /* body: SignatureScheme(2) || signature<0..2^16-1> */
                c->cv_alg = (uint16_t)((m[4] << 8) | m[5]);
                size_t slen = ((size_t)m[6] << 8) | m[7];
                if (slen <= sizeof c->cv_sig && 8 + slen <= total) {
                    memcpy(c->cv_sig, m + 8, slen); c->cv_sig_len = slen;
                }
                tls_transcript_update(&c->tr, m, total);
                off += total;
                continue;
            }
            /* EncryptedExtensions and anything else: advance the transcript. */
            tls_transcript_update(&c->tr, m, total);
            off += total;
        }
        if (off) { memmove(c->hbuf, c->hbuf + off, c->hlen - off); c->hlen -= off; }
    }

    /* AUTHENTICATE the peer (T3): the server Finished proved the handshake is
     * intact, but not WHO we're talking to. Verify the certificate chain to a
     * trusted anchor (+ hostname + validity) and the CertificateVerify signature
     * before sending our Finished or exchanging any application data. */
    int vrc = tls_verify_peer(c, server_name);
    if (vrc != 0) return -100 + vrc;   /* -101 host, -102 expired, -103/-104/-105
                                        * chain, -120/-121/-122 CertificateVerify,
                                        * -130 cert-message parse */
    c->verified = 1;

    /* Our Finished, over Hash(CH..server Finished), under the client hs key. */
    uint8_t th_after[32], cfin[36];
    tls_transcript_hash(&c->tr, th_after);
    cfin[0] = TLS_HS_FINISHED; cfin[1] = 0; cfin[2] = 0; cfin[3] = 32;
    tls_finished_mac(c->c_hs, th_after, cfin + 4);
    int rn = tls_record_seal(&c->tx, TLS_CT_HANDSHAKE, cfin, sizeof cfin,
                             c->recbuf, sizeof c->recbuf);
    if (rn < 0 || io_send_all(c->fd, c->recbuf, (size_t)rn)) return -1;

    /* Application traffic keys, over Hash(CH..server Finished). */
    uint8_t derived2[32], master[32], c_ap[32], s_ap[32];
    tls_derive_secret(c->hs_secret, "derived", empty_hash, derived2);
    tls_extract(derived2, NULL, master);
    tls_derive_secret(master, "c ap traffic", th_after, c_ap);
    tls_derive_secret(master, "s ap traffic", th_after, s_ap);
    install_keys(&c->tx, c_ap);
    install_keys(&c->rx, s_ap);

    c->established = 1;
    return 0;
}

/* ---- application data --------------------------------------------------- */

long tls_write(struct tls_conn *c, const void *buf, size_t len) {
    if (!c->established) return -1;
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > TLS_RECORD_MAX_PLAINTEXT) n = TLS_RECORD_MAX_PLAINTEXT;
        /* The two versions frame a record differently -- explicit nonce and
         * a different AAD in 1.2 -- so this is the one place the split shows
         * after the handshake. */
        int rn = c->version == 12
               ? tls12_seal(c, TLS_CT_APPLICATION_DATA, p + off, n,
                            c->recbuf, sizeof c->recbuf)
               : tls_record_seal(&c->tx, TLS_CT_APPLICATION_DATA, p + off, n,
                                 c->recbuf, sizeof c->recbuf);
        if (rn < 0 || io_send_all(c->fd, c->recbuf, (size_t)rn)) return -1;
        off += n;
    }
    return (long)len;
}

long tls_read(struct tls_conn *c, void *buf, size_t cap) {
    if (!c->established) return -1;

    /* Hand back any buffered leftover from a previous record first. */
    if (c->rleft_len) {
        size_t n = c->rleft_len < cap ? c->rleft_len : cap;
        memcpy(buf, c->rleft + c->rleft_off, n);
        c->rleft_off += n; c->rleft_len -= n;
        return (long)n;
    }

    for (;;) {
        int rl = recv_record(c);
        if (rl < 0) return -1;
        if (c->recbuf[0] == TLS_CT_CHANGE_CIPHER_SPEC) continue;
        /* In 1.2 the record header carries the REAL type, so an alert or a
         * post-handshake message arrives labelled as itself; 1.3 hides
         * everything behind application_data. */
        if (c->version != 12 && c->recbuf[0] != TLS_CT_APPLICATION_DATA) return -1;

        uint8_t typ;
        int n = c->version == 12
              ? tls12_open(c, c->recbuf, (size_t)rl,
                           c->rleft, sizeof c->rleft, &typ)
              : tls_record_open(&c->rx, c->recbuf, (size_t)rl,
                                c->rleft, sizeof c->rleft, &typ);
        if (n < 0) return -1;
        if (typ == TLS_CT_HANDSHAKE) continue;     /* NewSessionTicket etc: ignore */
        if (typ == TLS_CT_ALERT) return 0;         /* close_notify / any alert: EOF */
        if (typ != TLS_CT_APPLICATION_DATA) return -1;

        size_t give = (size_t)n;
        size_t m = give < cap ? give : cap;
        memcpy(buf, c->rleft, m);
        c->rleft_off = m; c->rleft_len = give - m;
        return (long)m;
    }
}

void tls_close(struct tls_conn *c) {
    if (c->established) {
        uint8_t alert[2] = { 1, 0 };   /* warning(1), close_notify(0) */
        /* THE THIRD PLACE THAT HAS TO KNOW THE VERSION, and the one that was
         * missed. On a 1.2 connection c->tx was never initialised -- those
         * keys live in tls12.c -- so sealing with it ran AES-GCM over a zeroed
         * context and called through a null pointer. The browser fetched the
         * page and then died closing the socket, which is why it looked like a
         * startup fault with no TLS anywhere near it. */
        int rn = c->version == 12
               ? tls12_seal(c, TLS_CT_ALERT, alert, 2, c->recbuf, sizeof c->recbuf)
               : tls_record_seal(&c->tx, TLS_CT_ALERT, alert, 2, c->recbuf, sizeof c->recbuf);
        if (rn > 0) (void)io_send_all(c->fd, c->recbuf, (size_t)rn);
    }
    close(c->fd);
}

/* The 1.2 path's clock, same source as 1.3's. */
void tls12_now_utc14(char out[15]) { now_utc14(out); }
