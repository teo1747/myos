/* user/web/css/css.h -- the CSS engine's contract.
 *
 * Split into three files because there are genuinely three concerns here, and
 * they fail in different ways:
 *
 *   decl.c   what a declaration MEANS -- "color: red" -> a field in vstyle.
 *            Pure string->value work. Shared by inline style="" and by rules.
 *   sel.c    what a selector MATCHES, and how strongly. Tree questions and
 *            specificity arithmetic. Knows nothing about properties.
 *   sheet.c  the CASCADE: parse a stylesheet into rules, then for one element
 *            decide which rules win and in what order they apply.
 *
 * The seam docs/BROWSER.md promised holds: all of this writes into the SAME
 * `struct vstyle` the user-agent stylesheet already filled. Nothing downstream
 * -- not the renderer, not layout, not the parser -- changes because CSS
 * arrived. The stylist just has more sources to consult.
 *
 * Bounded like everything else that touches network bytes: a fixed rule table,
 * fixed selector parts, no allocation. An oversized stylesheet loses its tail
 * and says so, rather than growing without limit.
 */
#ifndef _EMBLINK_WEB_CSS_H_
#define _EMBLINK_WEB_CSS_H_

#include <stddef.h>
#include "style.h"

/* ---- decl.c: one declaration block ------------------------------------- */

/* Apply the declarations in `text` ("color:red; font-weight:bold") to `out`.
 * Unknown properties and unparsable values are SKIPPED, never fatal -- CSS's
 * own error handling, and the only sane policy for bytes from a stranger.
 * Returns how many declarations were understood. */
int css_apply_decls(const char *text, size_t len, struct vstyle *out);

/* ---- sel.c: one compound selector --------------------------------------- */

/* Compounds kept per selector. Across seventeen real sites, 99.8% of the
 * 51503 selectors use four or fewer and 99.97% use six or fewer -- so six is
 * where the cost stops buying anything.
 *
 * What matters more than the number is WHICH ones are kept when a selector is
 * longer: see css_sel_parse. Keeping the first six would apply the rule to an
 * ANCESTOR of the element the author meant. */
#define CSS_SEL_PARTS 6

/* How a compound is joined to the one BEFORE it in the selector. Stored on the
 * later part, because matching runs right-to-left and that is the direction
 * the question gets asked in: "given this element, what must its parent /
 * previous sibling / some ancestor be?" */
enum {
    CSS_COMB_DESC = 0,   /* "a b"  -- any ancestor   */
    CSS_COMB_CHILD,      /* "a > b" -- the parent    */
    CSS_COMB_ADJ,        /* "a + b" -- the previous element sibling */
    CSS_COMB_SIB,        /* "a ~ b" -- any earlier sibling */
};

struct css_sel_part {
    char tag[16];                /* "" = any (or '*')          */
    char klass[32];              /* "" = none                  */
    char id[32];                 /* "" = none                  */
    unsigned char comb;          /* CSS_COMB_* joining to the previous part */
    unsigned char first_child;   /* :first-child */
    unsigned char last_child;    /* :last-child  */
};

struct css_sel {
    struct css_sel_part part[CSS_SEL_PARTS];  /* ancestor -> ... -> subject */
    unsigned char n;
    /* The selector names a PSEUDO-ELEMENT (::before, ::after, ::marker...) --
     * a box the document does not contain. It can never match a real element,
     * and treating it as if it did is not a near miss: the commonest rule on
     * the web is the clearfix
     *
     *     .container:after { content: ""; display: table; clear: both }
     *
     * and applying that to the container ITSELF turns a page into a blank
     * screen. python.org rendered zero words for exactly this reason.
     *
     * Deliberately NOT the same treatment as an unknown pseudo-CLASS: `a:hover`
     * styling `a` is closer to right than dropping the rule, because :hover
     * describes the same element in another state. A pseudo-element describes a
     * different box. */
    unsigned char pseudo_elem;
    unsigned short spec;         /* specificity: id*100 + class*10 + type */
};

/* ---- vars.c: CSS custom properties ------------------------------------- *
 * Document-scoped: one table for the whole sheet, last definition wins. See
 * vars.c for why, and for what that gives up. */
void        css_vars_reset(void);
void        css_vars_collect(const char *text, size_t len);   /* find `--x: y` */
void        css_var_set(const char *name, size_t nn, const char *val, size_t vn);
const char *css_var_get(const char *name, size_t nn);
/* Expand var() in a value. Returns 1 if anything was substituted. */
int         css_var_expand(const char *val, size_t vn, char *out, size_t cap);

/* ---- media.c: @media ---------------------------------------------------- *
 * The environment is set by the APP (the window's content width, and whether
 * the desktop is dark), because the browser is not the only thing that will
 * want to ask. Set it before parsing a sheet; a resize means parsing again. */
void css_media_set(float w, float h, int dark);
/* ...and read back, because vw/vh are the same environment a media query asks
 * about and must not be a second copy of it. */
float css_viewport_w(void);
float css_viewport_h(void);

/* ---- calc.c ------------------------------------------------------------- *
 * Reduce a calc() to a LINEAR EXPRESSION -- `*out_pct` percent of the
 * containing block plus `*out_px` pixels -- because the percentage cannot be
 * resolved until layout knows the container. Returns 0, or -1 if the value is
 * not a calc() or is not a length. */
int css_calc(const char *s, size_t n, float *out_pct, float *out_px);
int  css_media_matches(const char *query, size_t n);

/* Parse ONE selector ("nav ul li.item"). Returns 0, or -1 if unusable. */
int css_sel_parse(const char *s, size_t len, struct css_sel *out);

/* Does `sel` match element `node` in `doc`? Descendant combinator only
 * (whitespace); '>' '+' '~' are parsed as descendant, which is wrong in the
 * safe direction -- a slightly over-eager match, never a missed one. */
struct html_doc;
int css_sel_match(const struct css_sel *sel, struct html_doc *doc, int node);

/* ---- sheet.c: the stylesheet + cascade ---------------------------------- */

/* Set by MEASUREMENT, not by a round number. Across seventeen real sites the
 * median page needs 132 rules and 256 was fine for eleven of them -- but bbc
 * wants 781, mdn 1020, rust-lang 2570 and python.org 4314, and a page styled by
 * an arbitrary first 256 of its rules is styled arbitrarily: whatever came
 * after is simply not applied, so the cascade the author wrote is not the one
 * that runs.
 *
 * 4096 covers sixteen of the seventeen. The seventeenth is github, which ships
 * 6.9MB of CSS and 40942 rules; that is not a cap to chase, and hitting one is
 * still reported rather than silent (css_sheet.truncated).
 *
 * The bound exists because a page can be hostile -- so it stays a bound. It is
 * just no longer a bound that ordinary pages trip over. */
#define CSS_MAX_RULES 4096

struct css_rule {
    struct css_sel sel;
    const char    *decls;        /* into the caller's stylesheet text */
    size_t         decls_len;
    unsigned short order;        /* document order, for equal specificity */
};

/* THE SELECTOR INDEX.
 *
 * Without one, styling an element tests it against every rule in the sheet, so
 * a page costs rules x elements. That was invisible while sheets were capped
 * at 256 rules and became the browser's dominant cost the moment the cap was
 * raised to fit real ones: Wikipedia spent 315ms per build+layout pass on a
 * dev host, which under TCG on the metal is seconds a frame.
 *
 * Every rule is filed under ONE key -- taken from its subject (the rightmost
 * compound, the element the rule is actually about), choosing the most
 * selective thing that compound names: its id, else one of its classes, else
 * its tag, else nothing at all. An element then looks in exactly the buckets
 * that could hold a rule it matches: its own id, each of its classes, its tag,
 * and the keyless ones. A rule filed under class "banner" cannot match an
 * element without that class, so not looking is not a shortcut -- it is the
 * same answer for less work.
 *
 * One bucket per rule means no rule is visited twice and there is nothing to
 * de-duplicate. The cascade after that is unchanged: the survivors are still
 * sorted by (specificity, document order) and applied weakest-first. */
#define CSS_BUCKETS 1024        /* power of two; masked, not divided */

struct css_sheet {
    struct css_rule rules[CSS_MAX_RULES];
    int  n;
    int  truncated;              /* ran out of rule slots */

    /* Chains are rule indices, stored +1 so that 0 means "end". Built once by
     * css_sheet_index() after parsing rather than maintained per insert. */
    unsigned short bucket[CSS_BUCKETS];
    unsigned short next[CSS_MAX_RULES];
    unsigned short keyless;      /* rules whose subject names nothing */
    int  indexed;
};

/* Parse a stylesheet. `text` must OUTLIVE the sheet: rules point into it
 * (the same borrow-don't-copy discipline the DOM uses for its arena). */
void css_sheet_parse(struct css_sheet *sheet, const char *text, size_t len);

/* Build the selector index. css_sheet_parse does this itself; it is exposed
 * for anything that populates a sheet another way. Applying an UNINDEXED sheet
 * still works -- it falls back to testing every rule -- so forgetting this
 * costs speed and never correctness. */
void css_sheet_index(struct css_sheet *sheet);

/* A grid-area name as 16 bits -- see style.h. */
unsigned short css_name_hash(const char *s, size_t n);

/* Apply every matching rule to `out`, weakest first, so the strongest wins by
 * landing last. Call AFTER the user-agent stylesheet and BEFORE inline style,
 * which is the cascade's origin order. */
void css_sheet_apply(const struct css_sheet *sheet, struct html_doc *doc,
                     int node, struct vstyle *out);

#endif /* _EMBLINK_WEB_CSS_H_ */
