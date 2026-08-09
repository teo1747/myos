/* user/web/charset.h -- what the bytes of a document MEAN.
 *
 * Everything downstream of the fetch assumes UTF-8: the parser walks it, the
 * shaper decodes it into codepoints, the font draws them. That assumption is
 * wrong on a large part of the web, and wrong in a way that looks like a font
 * bug rather than a decoding one -- google.com serves this browser
 * `charset=ISO-8859-1`, in which an e-acute is the single byte 0xE9. As UTF-8
 * that is an invalid continuation, so the decoder yields U+FFFD and the page
 * reads "Confidentialit<?>" with a replacement character exactly where every
 * accent should be. The page is not broken and neither is the font; nobody
 * asked what encoding the bytes were in.
 *
 * So this module answers one question -- what encoding is this document -- and
 * performs one conversion. It is deliberately not an iconv: the legacy web that
 * still matters is ISO-8859-1 and windows-1252, and browsers treat the first as
 * the second (the C1 range 0x80-0x9F carries smart quotes and dashes in real
 * pages, and decoding those as control characters loses them). Anything else
 * declared is left alone rather than guessed at, because a wrong transcoding
 * destroys text that would otherwise merely look odd.
 *
 * No I/O and no allocation: the conversion happens in the caller's buffer, so
 * it is testable on a host and cannot fail for want of memory.
 */
#ifndef _EMBLINK_WEB_CHARSET_H_
#define _EMBLINK_WEB_CHARSET_H_

#include <stddef.h>

/* The `charset=` parameter of a Content-Type value ("text/html; charset=X"),
 * lowercased into `out`. Returns 1 if one was found. */
int charset_from_content_type(const char *content_type, char *out, size_t cap);

/* The encoding a DOCUMENT declares about itself: <meta charset="x"> or
 * <meta http-equiv="Content-Type" content="...; charset=x">. Only the first
 * `limit` bytes are examined, because that is where a declaration is required
 * to be and scanning a megabyte for one that is not there is waste. Returns 1
 * if found. */
int charset_from_meta(const char *src, size_t len, char *out, size_t cap);

/* Does `name` name an encoding we must convert FROM? UTF-8 and US-ASCII need
 * nothing (ASCII is a subset). An unknown name returns 0 -- left alone, on the
 * grounds that most of the web is UTF-8 and a wrong conversion is worse than
 * none. */
int charset_needs_transcode(const char *name);

/* Are these bytes valid UTF-8? Used only when NOTHING declared an encoding --
 * no HTTP header, no <meta> -- where a browser is left to sniff. A document
 * that declares UTF-8 and then breaks it is honoured as declared (and shows
 * replacement characters, as it should); it is the SILENT legacy page, of
 * which there are many, that this rescues. */
int charset_valid_utf8(const char *src, size_t len);

/* THE POLICY, in one place so every caller applies the same one. `declared` is
 * whatever the HTTP header said, else what the document's <meta> said, else
 * NULL. Returns 1 when the bytes should be read as windows-1252.
 *
 * Two cases, and the second is a deliberate departure from the letter of the
 * spec. A declared single-byte encoding is converted, plainly. But a page that
 * declares UTF-8 (or declares nothing) and then contains bytes that CANNOT be
 * UTF-8 is converted too -- google.com does exactly this, serving
 * `Content-Type: charset=ISO-8859-1` and a `<meta charset=utf-8>` over
 * Latin-1 bytes. The spec's answer is a replacement character per accent; the
 * useful answer is the text the author wrote, and a document already
 * self-contradictory has no third reading to protect. */
int charset_should_transcode(const char *declared, const char *src, size_t len);

/* Convert `len` bytes of windows-1252 in `buf` to UTF-8, in place, growing into
 * `cap`. Returns the new length, or 0 if the result would not fit -- in which
 * case the buffer is UNTOUCHED and the caller renders the original bytes,
 * which is the same as today rather than worse.
 *
 * Every byte below 0x80 is itself, so an ASCII document is unchanged and the
 * common case costs one pass and no growth. */
size_t charset_1252_to_utf8(char *buf, size_t len, size_t cap);

#endif /* _EMBLINK_WEB_CHARSET_H_ */
