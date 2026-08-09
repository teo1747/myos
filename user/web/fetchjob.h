/* user/web/fetchjob.h -- a fetch that does not freeze the window.
 *
 * net.c answers "what are the bytes for this location". This answers a
 * different question -- WHEN, and who waits for it -- and that is why it is its
 * own file. A GUI event loop cannot block: a TLS handshake to a real host is
 * several round trips, and every one of them is a frame the window does not
 * draw. The user sees a dead application.
 *
 * So the blocking fetch runs on a worker thread and the view polls. The whole
 * design rests on one property, checked rather than assumed: the fetch path is
 * ALLOCATION-FREE (net.c keeps its one TLS context static, and libtls itself
 * never allocates), so the worker and the UI thread never touch newlib's
 * allocator at the same time -- which matters because __malloc_lock is a stub
 * in this libc and malloc here is not thread-safe.
 *
 * ONE job at a time, on purpose. A browser navigates to one page at a time, and
 * a second concurrent fetch would need a second static TLS context and an
 * answer to "which one wins". Starting a fetch while one is in flight is
 * refused, and the caller shows that it is still loading.
 */
#ifndef _EMBLINK_WEB_FETCHJOB_H_
#define _EMBLINK_WEB_FETCHJOB_H_

#include <stddef.h>
#include "net.h"

/* Start fetching `url` into `buf`. The buffer belongs to the JOB until it
 * finishes -- do not touch it before fetchjob_poll reports done.
 * `tag` identifies the CALLER: a browser has two of them (the document and
 * the page's images) sharing one worker, and without a tag whichever polled
 * first would consume the other's result -- a picture would be handed to the
 * HTML parser. Returns 0, or -1 if a fetch is already in flight. */
int fetchjob_start(const char *url, char *buf, size_t cap, int tag);

/* WHERE THE SECONDS WENT, per tag: total bytes, total milliseconds, and how
 * many fetches. A slow page is the loudest complaint a browser has, and a
 * single "Loading... 146s" cannot say whether that is one big transfer or a
 * hundred small ones. Reset when a navigation starts. */
void fetchjob_stats(int tag, unsigned long *bytes, unsigned long *ms, unsigned *n);
void fetchjob_stats_reset(void);

/* ...with a POST body. `body` is COPIED, because the caller's buffer (a form's
 * encoded fields) is not guaranteed to outlive the worker. */
int fetchjob_start_post(const char *url, const char *body,
                        char *buf, size_t cap, int tag);

/* Poll for a result belonging to `tag`.
 *   1 = finished (written to *out, buffer is yours again, job consumed)
 *   0 = nothing for you yet -- either still running, or DONE but owned by
 *       someone else, which must NOT be consumed here
 *  -1 = nothing running at all
 *
 * Consumption is per-owner and that is the whole point. Reporting the tag but
 * consuming regardless looks like it works and silently destroys the other
 * caller's result: the browser's document poll ate every image completion, so
 * pictures stayed "loading" forever with no error anywhere. */
int fetchjob_poll(int tag, struct vnet_result *out);

/* Is a fetch in flight right now? For the view's loading state. */
int fetchjob_busy(void);

/* Milliseconds since the current job started -- so a slow page can say so
 * rather than looking hung. 0 when idle. */
unsigned fetchjob_elapsed_ms(void);

/* What is being fetched, for the loading message. "" when idle. */
const char *fetchjob_url(void);

#endif /* _EMBLINK_WEB_FETCHJOB_H_ */
