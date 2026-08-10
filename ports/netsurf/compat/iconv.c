/* ports/netsurf/compat/iconv.c -- the three iconv functions, and only what
 * a browser actually converts.
 *
 * libparserutils declares its own codecs for the encodings that matter --
 * US-ASCII, the ISO-8859 family, UTF-8, UTF-16 -- and reaches for iconv only
 * when a document declares something outside that set. Our newlib ships the
 * HEADER but not the implementation (iconv is a newlib configure option we do
 * not build), so the symbols were the last three missing pieces of the whole
 * NetSurf library stack.
 *
 * This is NOT an iconv. A real one carries a table per encoding and a state
 * machine for the stateful ones, and pretending to that while quietly
 * mangling Shift-JIS would be worse than saying no: a wrong transcoding
 * destroys text that would otherwise merely look odd. What it does is the
 * conversion the legacy web still needs -- windows-1252 (and ISO-8859-1,
 * which every browser decodes AS windows-1252, because the 0x80-0x9F range
 * carries smart quotes and dashes in real pages) to and from UTF-8, plus the
 * identity cases -- and returns EINVAL for anything else, which is exactly
 * what iconv_open is specified to do for a pair it cannot handle. parserutils
 * then reports the document's charset as unsupported instead of decoding it
 * wrongly.
 *
 * The mapping tables are user/web/charset.c's, which the browser before this
 * one proved on real pages.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <iconv.h>

/* Which side of the conversion an encoding is. Deliberately tiny. */
enum enc {
    ENC_UNSUPPORTED = 0,
    ENC_UTF8,
    ENC_1252,          /* windows-1252, and ISO-8859-1 decoded as it */
    ENC_ASCII,
};

/* iconv_open takes names with arbitrary punctuation and case, and the same
 * encoding arrives spelled six ways ("UTF-8", "utf8", "UTF-8//TRANSLIT"). */
static enum enc enc_of(const char *name)
{
    char n[32];
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof n; i++) {
        char c = name[i];
        if (c == '/') break;                     /* //TRANSLIT, //IGNORE */
        if (c == '-' || c == '_' || c == ' ' || c == '.') continue;
        n[j++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    n[j] = '\0';

    if (!strcmp(n, "utf8") || !strcmp(n, "utf8mb4")) return ENC_UTF8;
    if (!strcmp(n, "usascii") || !strcmp(n, "ascii") || !strcmp(n, "ansix341968"))
        return ENC_ASCII;
    if (!strcmp(n, "windows1252") || !strcmp(n, "cp1252") ||
        !strcmp(n, "iso88591")    || !strcmp(n, "latin1") ||
        !strcmp(n, "iso885915")   || !strcmp(n, "l1"))
        return ENC_1252;
    return ENC_UNSUPPORTED;
}

/* A descriptor is just the pair, packed into the pointer-sized handle iconv_t
 * already is -- no allocation, so iconv_open cannot fail for want of memory
 * and iconv_close cannot leak. */
#define CD_MAKE(f, t)  ((iconv_t)(uintptr_t)(((unsigned)(f) << 8) | (unsigned)(t)))
#define CD_FROM(cd)    ((enum enc)(((uintptr_t)(cd) >> 8) & 0xFF))
#define CD_TO(cd)      ((enum enc)((uintptr_t)(cd) & 0xFF))

/* windows-1252's C1 range. The other 224 code points are their own value,
 * which is what makes ISO-8859-1 a subset; only 0x80-0x9F differ, and those
 * are the ones real pages use for quotes and dashes. */
static const uint16_t c1_to_ucs[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    enum enc f = enc_of(fromcode), t = enc_of(tocode);
    if (f == ENC_UNSUPPORTED || t == ENC_UNSUPPORTED) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    return CD_MAKE(f, t);
}

int iconv_close(iconv_t cd)
{
    (void)cd;
    return 0;
}

/* Encode one code point as UTF-8 into at most *outleft bytes. Returns the
 * length written, or 0 when it does not fit. */
static size_t utf8_put(unsigned cp, char *out, size_t space)
{
    if (cp < 0x80)    { if (space < 1) return 0; out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { if (space < 2) return 0;
                        out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (space < 3) return 0;
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft)
{
    if (cd == (iconv_t)-1) { errno = EBADF; return (size_t)-1; }
    /* The reset call: no input buffer means "return to the initial state",
     * and every conversion here is stateless, so there is nothing to do. */
    if (inbuf == NULL || *inbuf == NULL) return 0;

    enum enc from = CD_FROM(cd), to = CD_TO(cd);
    const unsigned char *in = (const unsigned char *)*inbuf;
    char *out = *outbuf;
    size_t il = *inbytesleft, ol = *outbytesleft;
    size_t converted = 0;
    int err = 0;

    while (il > 0) {
        unsigned cp;
        size_t used;

        if (from == ENC_UTF8) {
            /* Decode one sequence, and REFUSE a malformed one rather than
             * substituting: the caller is entitled to know its input is not
             * what it said it was. An incomplete tail at the end of a chunk
             * is EINVAL (come back with more), a bad byte is EILSEQ. */
            unsigned char c = in[0];
            if (c < 0x80)               { cp = c; used = 1; }
            else if ((c & 0xE0) == 0xC0) { used = 2; cp = c & 0x1Fu; }
            else if ((c & 0xF0) == 0xE0) { used = 3; cp = c & 0x0Fu; }
            else if ((c & 0xF8) == 0xF0) { used = 4; cp = c & 0x07u; }
            else                        { err = EILSEQ; break; }
            if (used > il)              { err = EINVAL; break; }
            for (size_t k = 1; k < used; k++) {
                if ((in[k] & 0xC0) != 0x80) { err = EILSEQ; break; }
                cp = (cp << 6) | (in[k] & 0x3Fu);
            }
            if (err) break;
        } else if (from == ENC_ASCII) {
            if (in[0] > 0x7F) { err = EILSEQ; break; }
            cp = in[0]; used = 1;
        } else {                                  /* ENC_1252 */
            cp = (in[0] >= 0x80 && in[0] <= 0x9F) ? c1_to_ucs[in[0] - 0x80] : in[0];
            used = 1;
        }

        size_t wrote;
        if (to == ENC_UTF8) {
            wrote = utf8_put(cp, out, ol);
            if (wrote == 0) { err = E2BIG; break; }
        } else if (to == ENC_ASCII) {
            if (cp > 0x7F) { err = EILSEQ; break; }
            if (ol < 1)    { err = E2BIG;  break; }
            *out = (char)cp; wrote = 1;
        } else {                                  /* to windows-1252 */
            int byte = -1;
            if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) byte = (int)cp;
            else for (int k = 0; k < 32; k++)
                if (c1_to_ucs[k] == cp) { byte = 0x80 + k; break; }
            if (byte < 0) { err = EILSEQ; break; }
            if (ol < 1)   { err = E2BIG;  break; }
            *out = (char)byte; wrote = 1;
        }

        in += used; il -= used;
        out += wrote; ol -= wrote;
        converted++;
    }

    /* Report progress even on failure -- the caller resumes from here, and a
     * converter that consumed input without saying so loses it. */
    *inbuf = (char *)(uintptr_t)in;
    *inbytesleft = il;
    *outbuf = out;
    *outbytesleft = ol;
    if (err) { errno = err; return (size_t)-1; }
    return converted;
}
