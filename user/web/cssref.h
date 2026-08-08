/* user/web/cssref.h -- fetching a document's EXTERNAL stylesheets.
 *
 * Nearly every real page keeps its CSS in a file and points at it with
 * <link rel=stylesheet>. Until this existed the browser parsed that link,
 * recorded nothing, and rendered the page with no author styling at all --
 * which is why a site whose markup we handled perfectly still came out looking
 * like a plain text dump.
 *
 * Same shape as imgcache: one fetch in flight on the worker fetchjob already
 * owns, pumped once per frame, bounded buffers, and a page whose sheets have
 * not arrived yet renders unstyled rather than not at all. A stylesheet is a
 * SUB-RESOURCE -- the document is worth showing before it is dressed.
 *
 * Cascade order is: every external sheet in document order, then the document's
 * own <style> blocks. That is not exactly what CSS says (the true order
 * interleaves <link> and <style> as they appear in the source), and it is
 * written down here rather than pretended away. It matches the overwhelmingly
 * common shape -- links in the head, inline <style> as the local override --
 * and getting it exactly right means the parser recording an ordinal for each,
 * which is a change to make when a page is found that needs it.
 */
#ifndef _EMBLINK_WEB_CSSREF_H_
#define _EMBLINK_WEB_CSSREF_H_

#include <stddef.h>

struct html_doc;

/* Forget everything; a new document is loading. */
void cssref_reset(void);

/* Begin fetching the sheets `doc` references, resolved against `base`.
 * Returns how many will be fetched. */
int cssref_start(struct html_doc *doc, const char *base);

/* Drive the fetch. Returns 1 when a sheet landed (the caller must rebuild its
 * cascade), 0 otherwise. Call once per frame. */
int cssref_pump(void);

/* Is a sheet still outstanding? */
int cssref_pending(void);

/* The concatenated text of every sheet that has arrived, in document order.
 * Valid until the next cssref_reset. */
const char *cssref_text(size_t *len);

/* Stylesheets dropped because they did not fit the buffers. A page missing a
 * whole sheet renders as though it had none, so this is worth showing. */
int cssref_dropped(void);

#endif /* _EMBLINK_WEB_CSSREF_H_ */
