/* ports/netsurf/compat/iconv_test.c -- the shim, on the host, in a second.
 *
 * The conversion a browser does most is the one nobody notices until it is
 * wrong: a page served ISO-8859-1 whose accents come out as replacement
 * characters. These are the cases that decide it, including the two ways a
 * partial conversion has to report progress -- a caller that resumes from the
 * wrong offset loses text, and that is invisible until a page is missing a
 * paragraph.
 *
 *   make iconv-test
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <iconv.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } \
                         else printf("  ok:   %s\n", m); } while (0)

static int conv(const char *to, const char *from, const char *in, size_t inlen,
                char *out, size_t outcap, size_t *outlen) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return -1;
    char *ip = (char *)in, *op = out;
    size_t il = inlen, ol = outcap;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    *outlen = outcap - ol;
    return (r == (size_t)-1) ? -2 : 0;
}

int main(void) {
    char out[64]; size_t n;
    /* windows-1252 -> UTF-8: the whole reason this exists */
    const char latin[] = { (char)0xE9, (char)0xE8, 0 };            /* e-acute, e-grave */
    CHECK(conv("UTF-8", "ISO-8859-1", latin, 2, out, sizeof out, &n) == 0 && n == 4 &&
          (unsigned char)out[0] == 0xC3 && (unsigned char)out[1] == 0xA9,
          "ISO-8859-1 e-acute becomes two UTF-8 bytes");
    /* the C1 range every browser treats as windows-1252 */
    const char smart[] = { (char)0x93, (char)0x94, (char)0x96, 0 }; /* “ ” en-dash */
    CHECK(conv("UTF-8", "ISO-8859-1", smart, 3, out, sizeof out, &n) == 0 && n == 9,
          "the C1 range decodes as windows-1252, not as controls");
    /* round trip */
    char back[64]; size_t bn;
    CHECK(conv("UTF-8", "windows-1252", smart, 3, out, sizeof out, &n) == 0 &&
          conv("WINDOWS-1252", "UTF-8", out, n, back, sizeof back, &bn) == 0 &&
          bn == 3 && memcmp(back, smart, 3) == 0, "round trip is the same bytes");
    /* name spelling: punctuation and case are noise */
    CHECK(iconv_open("utf8", "cp1252") != (iconv_t)-1 &&
          iconv_open("UTF-8//TRANSLIT", "Latin1") != (iconv_t)-1,
          "names are matched loosely (utf8, cp1252, //TRANSLIT)");
    /* an encoding we cannot do must SAY SO, not guess */
    CHECK(iconv_open("UTF-8", "Shift_JIS") == (iconv_t)-1 && errno == EINVAL,
          "an unsupported encoding is refused with EINVAL");
    /* a short output buffer reports E2BIG and how far it got */
    {
        iconv_t cd = iconv_open("UTF-8", "ISO-8859-1");
        char small[3]; char *ip = (char *)latin, *op = small;
        size_t il = 2, ol = 3;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        CHECK(r == (size_t)-1 && errno == E2BIG && il == 1 && (size_t)(op - small) == 2,
              "a full output buffer stops with E2BIG and reports progress");
        iconv_close(cd);
    }
    /* malformed UTF-8 in is a sequence error, not a silent replacement */
    {
        const char bad[] = { (char)0xC3, 0x28, 0 };
        CHECK(conv("ISO-8859-1", "UTF-8", bad, 2, out, sizeof out, &n) == -2 &&
              errno == EILSEQ, "malformed UTF-8 is EILSEQ, never substituted");
        const char cut[] = { (char)0xC3, 0 };
        CHECK(conv("ISO-8859-1", "UTF-8", cut, 1, out, sizeof out, &n) == -2 &&
              errno == EINVAL, "a sequence cut by the chunk end is EINVAL");
    }
    /* ASCII is exact both ways */
    CHECK(conv("UTF-8", "US-ASCII", "hello", 5, out, sizeof out, &n) == 0 &&
          n == 5 && memcmp(out, "hello", 5) == 0, "ASCII passes through unchanged");
    printf(fails ? "=== iconv-shim: FAIL (%d)\n" : "=== iconv-shim: OK (%d failures)\n", fails);
    return fails != 0;
}
