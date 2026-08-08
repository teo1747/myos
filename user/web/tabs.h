/* user/web/tabs.h -- more than one page at a time.
 *
 * A tab is not a window. Two windows is something the compositor already does
 * and the user already has; what a tab adds is that the OTHER page is still
 * there -- still at the paragraph you stopped reading, still with the URL you
 * typed -- while you look at this one. So a tab is precisely: what has to
 * survive while it is NOT on screen.
 *
 * WHAT A BACKGROUND TAB KEEPS, and why that list is short.
 *
 * The parsed document is one fixed arena -- 8192 nodes and 256KB of strings,
 * bounded because a browser can be handed a hostile page (docs/BROWSER.md §7).
 * Six of those is six times a bound chosen to be safe, which is a different
 * and much worse bound. So the arena is not per-tab: exactly one document is
 * parsed at a time, and it belongs to whichever tab you are looking at.
 *
 * What each tab keeps instead is the SOURCE it was built from -- the bytes
 * that already came off the network. Switching to a tab re-parses those bytes.
 * That costs a parse (milliseconds; the same parse the page cost when it
 * loaded) and it costs no network, no re-POST, and no chance of getting a
 * different page than the one you left. It is the same trade a desktop browser
 * makes when it discards a background tab, arrived at from the memory side
 * rather than the battery side.
 *
 * The honest consequence, stated here rather than discovered later: a
 * background tab's SCRIPTS do not keep running, and its DOM is rebuilt from
 * source when you come back. A clock in a background tab is not ticking, and
 * what a script wrote into the DOM -- or typed into a form -- is gone on
 * return. Real browsers keep that alive; this one will too when a document
 * arena can be allocated per tab rather than reserved for one. Until then the
 * limitation is the design, not a bug to be surprised by.
 *
 * Scroll position, zoom, title and the back/forward stack ARE per tab, because
 * those are the whole point: a tab you return to that has forgotten where you
 * were is a bookmark.
 *
 * No syscalls here -- this is bookkeeping, so it is testable on a host.
 */
#ifndef _EMBLINK_WEB_TABS_H_
#define _EMBLINK_WEB_TABS_H_

#include <stddef.h>

/* Bounded, like everything else that scales with what a page can ask for.
 * Each slot reserves its own source buffer, so this number is memory --
 * TAB_MAX * TAB_SRC_MAX bytes of it, reserved up front and never grown. */
#define TAB_MAX      6
#define TAB_SRC_MAX  (512 * 1024)
#define TAB_URL_MAX  512
#define TAB_TITLE_MAX 96
#define TAB_HIST_MAX 24

/* Start with one empty tab. Idempotent -- calling it twice does not add one. */
void tab_init(void);

int  tab_count(void);
/* Does slot `i` hold a tab? The strip iterates slots, and a new empty tab has
 * neither URL nor bytes yet -- so this is the only question that answers it. */
int  tab_is_open(int i);
int  tab_current(void);

/* Open a tab and return its index, or -1 when there is no room. It does NOT
 * become current: opening a link in a background tab is the common case, and
 * the caller says which it meant by calling tab_select. */
int  tab_open(const char *url);

/* Close a tab and return the index that is current AFTERWARDS. Refuses to
 * close the last one -- a browser with no tabs has nothing to show, and every
 * browser answers this the same way. */
int  tab_close(int i);

/* Make `i` current. Returns 0 if it changed, -1 if the index is bad or it was
 * already current (so the caller can skip a re-parse it does not need). */
int  tab_select(int i);

const char *tab_url(int i);
void        tab_set_url(int i, const char *u);
const char *tab_title(int i);
void        tab_set_title(int i, const char *t);
/* What to show on the tab itself: the title if the page gave one, else
 * something recognisable from the URL. Never empty -- a nameless tab is
 * unclickable. */
const char *tab_label(int i);

float tab_scroll(int i);
void  tab_set_scroll(int i, float y);
float tab_zoom(int i);
void  tab_set_zoom(int i, float z);

/* The retained source. tab_src() is writable so a fetch can land straight into
 * it without a copy. */
char  *tab_src(int i);
size_t tab_src_len(int i);
void   tab_set_src_len(int i, size_t n);
/* Copy bytes in (bounded); returns how many were kept. */
size_t tab_set_src(int i, const char *bytes, size_t n);

/* Per-tab back / forward. `url` is where you are NOW; push it when you leave.
 * Returns 0 and fills `out` when there is somewhere to go. */
void tab_hist_push(int i, const char *url);
int  tab_hist_back(int i, const char *current, char *out, size_t cap);
int  tab_hist_fwd(int i, const char *current, char *out, size_t cap);
int  tab_can_back(int i);
int  tab_can_fwd(int i);

#endif /* _EMBLINK_WEB_TABS_H_ */
