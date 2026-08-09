/* user/web/charset.c -- see charset.h. */

#include <string.h>
#include "charset.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* Case-insensitive "does `hay` contain `needle`", over a bounded span. */
static const char *find_ci(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && lower(hay[i + k]) == lower(needle[k])) k++;
        if (k == m) return hay + i;
    }
    return 0;
}

/* Copy the token that follows a `charset=`, stopping at whatever ends it. */
static int take_value(const char *p, size_t n, char *out, size_t cap) {
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '"' || p[i] == '\'')) i++;
    size_t o = 0;
    while (i < n && o + 1 < cap) {
        char c = p[i];
        if (c == '"' || c == '\'' || c == ';' || c == ' ' || c == '\t' ||
            c == '>' || c == '\r' || c == '\n' || c == ',') break;
        out[o++] = lower(c);
        i++;
    }
    out[o] = 0;
    return o > 0;
}

int charset_from_content_type(const char *ct, char *out, size_t cap) {
    if (!ct || !out || cap < 2) return 0;
    size_t n = strlen(ct);
    const char *p = find_ci(ct, n, "charset=");
    if (!p) return 0;
    p += 8;
    return take_value(p, (size_t)(ct + n - p), out, cap);
}

int charset_from_meta(const char *src, size_t len, char *out, size_t cap) {
    if (!src || !out || cap < 2) return 0;
    /* A declaration is required to be in the first kilobyte of the document;
     * looking further finds the word "charset" inside a script or a link and
     * believes it. */
    const size_t limit = 2048;
    size_t n = len < limit ? len : limit;

    /* Search inside <meta ...> only, so a `charset=` in a URL or a script does
     * not answer for the document. */
    for (size_t i = 0; i + 5 < n; i++) {
        if (src[i] != '<') continue;
        if (lower(src[i+1]) != 'm' || lower(src[i+2]) != 'e' ||
            lower(src[i+3]) != 't' || lower(src[i+4]) != 'a') continue;
        size_t j = i;
        while (j < n && src[j] != '>') j++;
        const char *p = find_ci(src + i, j - i, "charset=");
        if (p && take_value(p + 8, (size_t)(src + j - (p + 8)), out, cap)) return 1;
        i = j;
    }
    return 0;
}

int charset_needs_transcode(const char *name) {
    if (!name || !name[0]) return 0;
    static const char *const single_byte[] = {
        "iso-8859-1", "iso8859-1", "latin1", "latin-1", "l1",
        "windows-1252", "cp1252", "iso-8859-15", "iso8859-15",
        0
    };
    for (int i = 0; single_byte[i]; i++)
        if (!strcmp(name, single_byte[i])) return 1;
    return 0;   /* utf-8, us-ascii, and anything we do not know: leave alone */
}

/* The bytes windows-1252 puts where ISO-8859-1 has C1 controls. Real pages are
 * full of these -- smart quotes, en/em dashes, the euro -- and a browser that
 * decodes them as controls silently drops the punctuation out of the text.
 * Browsers all decode ISO-8859-1 this way; the spec says so too. */
static const unsigned short CP1252_C1[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

size_t charset_1252_to_utf8(char *buf, size_t len, size_t cap) {
    if (!buf) return 0;

    /* MEASURE FIRST, then move. The conversion grows the text, so writing it
     * forwards would overwrite bytes not yet read; and a result that does not
     * fit has to be discovered BEFORE anything is modified, or a document too
     * large to convert is left half-converted and unreadable in a new way. */
    size_t need = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) { need += 1; continue; }
        unsigned cp = (c >= 0x80 && c <= 0x9F) ? CP1252_C1[c - 0x80] : c;
        need += (cp < 0x800) ? 2 : 3;
    }
    if (need == len) return len;              /* pure ASCII: nothing to do */
    if (need + 1 > cap) return 0;             /* would not fit: leave it be */

    /* Backwards, so each byte is read before the expansion can reach it. */
    size_t w = need;
    buf[w] = 0;
    for (size_t i = len; i-- > 0; ) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) { buf[--w] = (char)c; continue; }
        unsigned cp = (c >= 0x80 && c <= 0x9F) ? CP1252_C1[c - 0x80] : c;
        if (cp < 0x800) {
            buf[--w] = (char)(0x80 | (cp & 0x3F));
            buf[--w] = (char)(0xC0 | (cp >> 6));
        } else {
            buf[--w] = (char)(0x80 | (cp & 0x3F));
            buf[--w] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[--w] = (char)(0xE0 | (cp >> 12));
        }
    }
    return need;
}

int charset_valid_utf8(const char *src, size_t len) {
    if (!src) return 1;
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)src[i];
        int n;
        if      (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return 0;                       /* a stray continuation or 0xF8+ */
        /* Not enough bytes left to finish the sequence: INCONCLUSIVE, not
         * invalid. Our own fetch caps large documents, so a perfectly good
         * UTF-8 page can arrive cut through a character -- and judging that
         * "not UTF-8" would transcode the whole document and mangle every
         * accent in it. A page whose only bad byte is its last is not worth
         * that risk. */
        if (i + (size_t)n >= len) return 1;
        for (int k = 1; k <= n; k++)
            if (((unsigned char)src[i + (size_t)k] & 0xC0) != 0x80) return 0;
        i += (size_t)n + 1;
    }
    return 1;
}

int charset_should_transcode(const char *declared, const char *src, size_t len) {
    if (declared && declared[0] && charset_needs_transcode(declared)) return 1;
    /* Declared UTF-8, or nothing declared: believe the BYTES when they make the
     * declaration impossible. */
    return !charset_valid_utf8(src, len);
}
