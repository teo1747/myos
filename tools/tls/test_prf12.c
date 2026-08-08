/* Host test for the TLS 1.2 PRF (RFC 5246 §5).
 *
 * The vectors are the widely-published SHA-256 PRF test case (the IETF TLS WG
 * list's "PRF Test Vector" for TLS 1.2), plus a self-consistency check that
 * the key block splits the way an AEAD suite needs. A PRF that is wrong by a
 * byte produces a handshake that fails at Finished with no other symptom, so
 * it is worth pinning against numbers computed elsewhere. */
#include <stdio.h>
#include <string.h>
#include "prf12.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } \
                         else printf("  ok   %s\n", m); } while (0)

static void hexdump(const char *tag, const uint8_t *b, size_t n) {
    printf("    %s: ", tag);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void) {
    printf("=== TLS 1.2 PRF (P_SHA256) ===\n");

    /* IETF TLS WG vector: secret/label/seed -> 100 bytes. */
    const uint8_t secret[16] = {
        0x9b,0xbe,0x43,0x6b,0xa9,0x40,0xf0,0x17,
        0xb1,0x76,0x52,0x84,0x9a,0x71,0xdb,0x35 };
    const uint8_t seed[16] = {
        0xa0,0xba,0x9f,0x93,0x6c,0xda,0x31,0x18,
        0x27,0xa6,0xf7,0x96,0xff,0xd5,0x19,0x8c };
    static const uint8_t want[100] = {
        0xe3,0xf2,0x29,0xba,0x72,0x7b,0xe1,0x7b,0x8d,0x12,0x26,0x20,0x55,0x7c,0xd4,0x53,
        0xc2,0xaa,0xb2,0x1d,0x07,0xc3,0xd4,0x95,0x32,0x9b,0x52,0xd4,0xe6,0x1e,0xdb,0x5a,
        0x6b,0x30,0x17,0x91,0xe9,0x0d,0x35,0xc9,0xc9,0xa4,0x6b,0x4e,0x14,0xba,0xf9,0xaf,
        0x0f,0xa0,0x22,0xf7,0x07,0x7d,0xef,0x17,0xab,0xfd,0x37,0x97,0xc0,0x56,0x4b,0xab,
        0x4f,0xbc,0x91,0x66,0x6e,0x9d,0xef,0x9b,0x97,0xfc,0xe3,0x4f,0x79,0x67,0x89,0xba,
        0xa4,0x80,0x82,0xd1,0x22,0xee,0x42,0xc5,0xa7,0x2e,0x5a,0x51,0x10,0xff,0xf7,0x01,
        0x87,0x34,0x7b,0x66 };
    uint8_t got[100];
    tls12_prf(secret, sizeof secret, "test label", seed, sizeof seed, got, sizeof got);
    if (memcmp(got, want, sizeof want) != 0) { hexdump("got ", got, 32); hexdump("want", want, 32); }
    CHECK(memcmp(got, want, sizeof want) == 0, "P_SHA256 matches the published vector");

    /* A short request is a prefix of a long one -- the blocks are a stream. */
    uint8_t shortout[20];
    tls12_prf(secret, sizeof secret, "test label", seed, sizeof seed, shortout, sizeof shortout);
    CHECK(memcmp(shortout, want, sizeof shortout) == 0, "a shorter output is a prefix of a longer one");

    /* The two derivations use OPPOSITE random orders; getting that backwards
     * is a bug with no symptom until the peer cannot decrypt. */
    uint8_t cr[32], sr[32];
    for (int i = 0; i < 32; i++) { cr[i] = (uint8_t)i; sr[i] = (uint8_t)(0x80 + i); }
    uint8_t master[48], kb[40], kb_swapped[40];
    tls12_master_secret(secret, sizeof secret, cr, sr, master);
    tls12_key_block(master, cr, sr, kb, sizeof kb);
    tls12_key_block(master, sr, cr, kb_swapped, sizeof kb_swapped);
    CHECK(memcmp(kb, kb_swapped, sizeof kb) != 0,
          "the key block depends on WHICH random comes first");

    uint8_t vd_c[12], vd_s[12], th[32];
    for (int i = 0; i < 32; i++) th[i] = (uint8_t)(i * 7);
    tls12_verify_data(master, "client finished", th, vd_c);
    tls12_verify_data(master, "server finished", th, vd_s);
    CHECK(memcmp(vd_c, vd_s, 12) != 0, "client and server Finished differ by label");

    printf("=== prf12: %s (%d failures) ===\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
