#ifndef EMBK_TLS_H
#define EMBK_TLS_H
/* libtls -- a TLS 1.3 client (RFC 8446) over a connected TCP socket fd, on our
 * own crypto. Public contract. See docs/TLS.md.
 *
 * v1 (T2) milestone: the handshake completes and the server's Finished MAC is
 * verified -- which proves the ECDHE, key schedule, transcript, and record layer
 * are all byte-correct -- but the server CERTIFICATE IS NOT VERIFIED (parsed and
 * ignored). That makes this encrypted-but-unauthenticated: safe against passive
 * eavesdroppers, NOT against an active man-in-the-middle. Certificate/hostname
 * verification is T3, and lands before any real consumer (pip/git) rides on it. */
#include <stdint.h>
#include <stddef.h>
#include "record.h"
#include "handshake.h"

/* One TLS connection. ~50 KB -- allocate on the heap, not the stack. Treat the
 * fields as private. */
struct tls_conn {
    int  fd;
    int  established;
    /* 13 or 12 -- which half of the client is driving. The two share the
     * socket, the certificate rules and the cipher, and nothing else.
     *
     * The 1.2 AEAD state deliberately does NOT live here: it belongs to
     * tls12.c, and putting it in this header dragged the 1.2 record layer's
     * types into every file that speaks TLS at all. */
    int  version;
    struct tls_transcript tr;
    struct tls_keys rx, tx;             /* current read/write AEAD (hs -> app) */
    uint8_t hs_secret[32];              /* Handshake Secret (for the app keys) */
    uint8_t c_hs[32], s_hs[32];         /* handshake traffic secrets (Finished) */

    size_t  hlen;                       /* bytes buffered in hbuf */
    uint8_t hbuf[16384];                /* reassembles the server handshake flight */

    size_t  rleft_off, rleft_len;       /* decrypted app data not yet returned */
    uint8_t rleft[TLS_RECORD_MAX_PLAINTEXT + 1];

    uint8_t recbuf[5 + TLS_RECORD_MAX_PLAINTEXT + 256 + TLS_TAG_LEN];  /* one wire record */

    /* Captured during the flight for certificate verification (T3). */
    uint8_t  certmsg[16384]; size_t certmsg_len;   /* the raw Certificate message */
    uint8_t  th_cert[32];                          /* transcript hash CH..Certificate */
    uint16_t cv_alg;                               /* CertificateVerify SignatureScheme */
    uint8_t  cv_sig[512]; size_t cv_sig_len;       /* CertificateVerify signature */
    int      verified;                             /* 1 once the peer is authenticated */
};

/* Run the TLS 1.3 handshake over an already-TCP-connected `fd`. `server_name`
 * is sent as SNI (required by virtual-hosted servers). Returns 0 on success
 * (connection ready for tls_read/tls_write), -1 on I/O or protocol error, -2 on
 * an unsupported HelloRetryRequest, -3 if the server's Finished MAC did not
 * verify. Does not take ownership of `fd` on failure. */
int tls_connect(struct tls_conn *c, int fd, const char *server_name);

/* Send application data (fragmented into records). Returns len, or -1. */
long tls_write(struct tls_conn *c, const void *buf, size_t len);

/* Receive application data. Returns bytes read (>0), 0 on clean close
 * (close_notify / EOF), or -1 on error. Skips post-handshake messages
 * (NewSessionTicket etc.). */
long tls_read(struct tls_conn *c, void *buf, size_t cap);

/* Best-effort close_notify, then close(fd). */
void tls_close(struct tls_conn *c);

#endif /* EMBK_TLS_H */
