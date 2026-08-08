/* user/web/render.h -- document + computed styles -> EmUI. See render.c.
 *
 * Reads `struct vstyle` and never a tag name, so CSS changes nothing here. */
#ifndef _EMBLINK_WEB_RENDER_H_
#define _EMBLINK_WEB_RENDER_H_

#include "html.h"

/* Emit the document as EmUI nodes. Call inside a frame, in a scroll view.
 * Returns the href of a link clicked THIS frame, or NULL -- the caller copies
 * it before navigating, because the string lives in the document arena that
 * navigation is about to reuse. */
const char *vellum_render(struct html_doc *doc, int root);

/* ...and the same with the document's own stylesheet applied. `sheet` may be
 * NULL, which is exactly vellum_render(). */
struct css_sheet;
const char *vellum_render_styled(struct html_doc *doc, int root,
                                 const struct css_sheet *sheet);

/* ...and with a BASE URL, so <img src> can be resolved the way links are.
 * Without it a page's pictures are only findable when its markup happens to
 * use absolute URLs, which almost none do. */
const char *vellum_render_page(struct html_doc *doc, int root,
                               const struct css_sheet *sheet, const char *base);

/* ...and with the COLUMN WIDTH, so an oversized picture is scaled to fit
 * instead of overflowing. 0 = do not clamp. */
const char *vellum_render_sized(struct html_doc *doc, int root,
                                const struct css_sheet *sheet, const char *base,
                                float content_w);

/* Non-NULL enables link rendering (words become clickable). */
void vellum_set_link_handler(void (*fn)(const char *href));

/* Event hooks. The renderer asks `has_listener` which elements a script cares
 * about and makes ONLY those clickable -- a browser where every container
 * swallows clicks is one whose links stop working. Both may be NULL (no
 * engine), which is exactly the pre-script behaviour. */
void vellum_set_event_hooks(int (*has_listener)(int node),
                            void (*on_click)(int node));

/* A form was submitted (a submit button pressed). The renderer knows WHEN;
 * only the app knows what navigating means. */
void vellum_set_submit_handler(void (*fn)(int node));

/* Which field had keyboard focus on the last rendered frame, or -1. The app
 * uses it to make Enter submit -- which is a FORM's idea, not a text box's,
 * so the toolkit rightly has no opinion about it. */
int  vellum_focused_field(void);

/* The link under the pointer, for a status line. NULL when none. */
const char *vellum_hovered_link(void);

/* Page zoom: 1.0 is unzoomed. Scales the document's text and the lengths its
 * author stated, and leaves the browser's own chrome alone. */
/* Forget which <details> the reader had open. Belongs with form_reset and
 * vsel_reset: it is UI state about the OLD document. */
void  vellum_reset_details(void);

/* DIAGNOSTIC: be told which instance box each element got, as it is emitted.
 * The pair is an instance handle (index, generation); resolve it after layout
 * to ask how big the element ended up. Exists because "the page is 300000
 * pixels tall" is a fact about the document that says nothing about which box
 * did it -- and the obvious way to find out, keying every block, would make
 * every div a hit target. NULL to stop. */
void  vellum_set_box_hook(void (*fn)(int node, unsigned idx, unsigned gen));

void  vellum_set_zoom(float z);
float vellum_zoom(void);

#endif
