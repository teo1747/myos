/* Host test for the TLS 1.2 AEAD record layer (RFC 5288).
 *
 * Round-trip plus the three things that differ from 1.3 and fail silently if
 * wrong: the explicit nonce travelling on the wire, the additional data being
 * seq||type||version||length rather than the record header, and the content
 * type coming from the header. */
#include <stdio.h>
#include <string.h>
#include "record12.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } \
                         else printf("  ok   %s\n", m); } while (0)

int main(void) {
    printf("=== TLS 1.2 record layer ===\n");
    uint8_t key[16], salt[4];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 4; i++)  salt[i] = (uint8_t)(0xA0 + i);

    struct tls12_keys w, r;
    tls12_keys_init(&w, key, salt);
    tls12_keys_init(&r, key, salt);

    const char *msg = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    uint8_t rec[512], out[512], type = 0;
    int n = tls12_record_seal(&w, TLS_CT_APPLICATION_DATA,
                              (const uint8_t *)msg, strlen(msg), rec, sizeof rec);
    CHECK(n > 0, "a record seals");
    CHECK(n == (int)(5 + 8 + strlen(msg) + 16),
          "...and is 8 bytes longer than 1.3 would be: the explicit nonce is on the wire");
    CHECK(rec[0] == TLS_CT_APPLICATION_DATA && rec[1] == 3 && rec[2] == 3,
          "the header carries the real content type, not a hidden one");

    int m = tls12_record_open(&r, rec, (size_t)n, out, sizeof out, &type);
    CHECK(m == (int)strlen(msg) && memcmp(out, msg, strlen(msg)) == 0, "...and opens back");
    CHECK(type == TLS_CT_APPLICATION_DATA, "the type comes from the header");

    /* Sequence numbers advance together; a second record must still open. */
    int n2 = tls12_record_seal(&w, TLS_CT_HANDSHAKE, (const uint8_t *)"hi", 2, rec, sizeof rec);
    int m2 = tls12_record_open(&r, rec, (size_t)n2, out, sizeof out, &type);
    CHECK(m2 == 2 && type == TLS_CT_HANDSHAKE, "the second record opens with seq advanced");

    /* A flipped bit anywhere must fail the tag, and write nothing. */
    struct tls12_keys w2, r2;
    tls12_keys_init(&w2, key, salt); tls12_keys_init(&r2, key, salt);
    n = tls12_record_seal(&w2, TLS_CT_APPLICATION_DATA, (const uint8_t *)msg, strlen(msg),
                          rec, sizeof rec);
    rec[20] ^= 0x01;
    memset(out, 0xEE, sizeof out);
    CHECK(tls12_record_open(&r2, rec, (size_t)n, out, sizeof out, &type) < 0,
          "a tampered record is refused");
    /* Nothing of the plaintext may be left in the caller's buffer -- a failed
     * open that half-decrypts is a failed open that leaks. */
    int leaked = 0;
    for (size_t i = 0; i < strlen(msg); i++) if (out[i] == (uint8_t)msg[i]) { leaked = 1; break; }
    CHECK(!leaked, "...and leaves no plaintext behind");

    printf("=== record12: %s (%d failures) ===\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
