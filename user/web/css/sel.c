/* user/web/css/sel.c -- what a selector MATCHES, and how strongly.
 *
 * Tree questions and specificity arithmetic. This file knows nothing about
 * properties: give it a selector and an element and it answers yes or no.
 * That separation is why the cascade in sheet.c stays short.
 *
 * Supported: type (`p`), class (`.item`), id (`#main`), universal (`*`),
 * compounds (`li.item`, `a#home.x`) and the descendant combinator (space).
 *
 * Combinators are REAL: `>` (child), `+` (adjacent sibling) and `~` (general
 * sibling) each mean what they say. They used to be parsed as descendant,
 * which over-matched -- `nav > ul` also styled a `ul` three levels down inside
 * a nav -- and an author who scoped a rule tightly got it applied widely. That
 * was the right first approximation (dropping the rule loses the intent
 * entirely) and it stops being right once pages are the target rather than
 * documents we wrote ourselves.
 *
 * Also supported: `:first-child` and `:last-child`.
 *
 * Also supported: `:is()`, `:where()` and `:not()`, which is what modern CSS
 * is written in -- across the seventeen real sites this browser is tested
 * against, `:where(` appears 4196 times, `:not(` 2693 and `:is(` 508. Before
 * this they were not merely unsupported, they were MIS-PARSED: the compound
 * scanner ran to the next space, and `:where(.a, .b)` has a space in it, so
 * `.b)` was read as a whole new compound and the rule was quietly attached to
 * the wrong element or to nothing. That is why pages built this decade came
 * out looking like pages from the last one.
 *
 * Every functional pseudo now consumes its balanced parentheses even when its
 * meaning is not implemented (`:nth-child(2n + 1)`, `:has(...)`), because the
 * mis-parse above was never about which ones we understand.
 *
 * Still NOT supported, and still skipped rather than dropped: attribute
 * selectors and every other pseudo-class. `a:hover` styling `a` is closer to
 * the author's page than no rule at all.
 */
#include <string.h>

#include "html.h"
#include "css.h"

/* The shared pool backing :is()/:where()/:not() argument lists. Bounded like
 * everything else a page can ask for: past the cap a compound simply carries no
 * such constraint, which LOOSENS the match rather than dropping the rule. */
static struct css_fn_arg g_fn[CSS_FN_ARGS];
static int g_fn_n;

void css_sel_pool_reset(void) { g_fn_n = 0; }

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

static void cpy_lower(char *dst, size_t cap, const char *s, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) dst[k++] = (char)ci(s[i]);
    dst[k] = 0;
}

/* Parse a compound selector chain, KEEPING THE RIGHTMOST compounds.
 *
 * The subject of a selector is its LAST compound -- the element the rule
 * actually applies to. Parsing straight into a fixed array and stopping when
 * it filled kept the leftmost instead, so a selector one compound too long was
 * applied to an ancestor of its target. With `display: none` on the end of it
 * that does not misplace a style, it DELETES A SUBTREE: five compounds deep,
 * a rule meant to hide one span hid the div four levels up and everything
 * inside it. Found rendering lobste.rs, which came out blank.
 *
 * So: parse the whole chain into scratch, then keep the tail. Dropping
 * ancestor constraints makes the selector match a SUPERSET of what it should
 * -- the wrong direction is to match a different element entirely. The first
 * kept combinator becomes a descendant for the same reason: it is the loosest,
 * and its real ancestor is no longer there to be checked. */

/* Parse the inside of :is()/:where()/:not() into the pool: alternatives split
 * on top-level commas, each read as a compound. An alternative that is itself a
 * complex selector (`.a .b`) keeps only its SUBJECT, and a nested functional
 * pseudo is skipped -- both loosen the match, which is the direction this file
 * has always erred in. Returns how many were stored and the highest
 * specificity among them, which is what :is() and :not() contribute. */
static int parse_fn_args(const char *s, size_t len, unsigned short *first,
                         unsigned short *max_spec) {
    *first = (unsigned short)g_fn_n;
    *max_spec = 0;
    int count = 0;
    size_t i = 0;
    while (i <= len) {
        /* one alternative, up to a top-level comma */
        size_t start = i, depth = 0;
        while (i < len && !(depth == 0 && s[i] == ',')) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')' && depth) depth--;
            i++;
        }
        size_t end = i;
        i++;                                   /* past the comma */

        if (g_fn_n >= CSS_FN_ARGS) break;
        struct css_fn_arg a;
        memset(&a, 0, sizeof a);
        unsigned spec = 0;
        int got = 0;
        for (size_t j = start; j < end; ) {
            if (is_ws(s[j]) || s[j] == '>' || s[j] == '+' || s[j] == '~') {
                /* a combinator inside the argument: restart, keeping the tail */
                memset(&a, 0, sizeof a); spec = 0; got = 0;
                j++;
                continue;
            }
            if (s[j] == '.' || s[j] == '#') {
                char kind = s[j++];
                size_t vs = j;
                while (j < end && !is_ws(s[j]) && s[j] != '.' && s[j] != '#' &&
                       s[j] != ':' && s[j] != '[') j++;
                if (j > vs) {
                    if (kind == '.') { cpy_lower(a.klass, sizeof a.klass, s + vs, j - vs); spec += 10; }
                    else             { cpy_lower(a.id, sizeof a.id, s + vs, j - vs);       spec += 100; }
                    got = 1;
                }
            } else if (s[j] == ':' || s[j] == '[') {
                /* a nested pseudo or an attribute test: consume it, including
                 * any balanced parentheses, and ignore what it meant */
                j++;
                while (j < end && !is_ws(s[j]) && s[j] != '.' && s[j] != '#' &&
                       s[j] != '(' && s[j] != ',') j++;
                if (j < end && s[j] == '(') {
                    size_t d = 0;
                    do { if (s[j] == '(') d++; else if (s[j] == ')') d--; j++; } while (j < end && d);
                }
            } else if (s[j] == '*') {
                j++; got = 1;
            } else {
                size_t vs = j;
                while (j < end && !is_ws(s[j]) && s[j] != '.' && s[j] != '#' &&
                       s[j] != ':' && s[j] != '[') j++;
                if (j > vs) { cpy_lower(a.tag, sizeof a.tag, s + vs, j - vs); spec += 1; got = 1; }
            }
        }
        if (got) {
            g_fn[g_fn_n++] = a;
            count++;
            if (spec > *max_spec) *max_spec = (unsigned short)spec;
        }
        if (i > len) break;
    }
    return count;
}

#define SEL_SCRATCH 24

int css_sel_parse(const char *s, size_t len, struct css_sel *out) {
    if (!s || !out) return -1;
    memset(out, 0, sizeof *out);

    struct css_sel_part scratch[SEL_SCRATCH];
    int nscratch = 0;
    memset(scratch, 0, sizeof scratch);

    size_t i = 0;
    while (i < len && nscratch < SEL_SCRATCH) {
        /* the combinator between the previous compound and this one; the last
         * one written wins, so "a  >  b" is a child combinator and not two */
        int comb = CSS_COMB_DESC;
        while (i < len && (is_ws(s[i]) || s[i]=='>' || s[i]=='+' || s[i]=='~')) {
            if      (s[i] == '>') comb = CSS_COMB_CHILD;
            else if (s[i] == '+') comb = CSS_COMB_ADJ;
            else if (s[i] == '~') comb = CSS_COMB_SIB;
            i++;
        }
        if (i >= len) break;

        struct css_sel_part *pt = &scratch[nscratch];
        pt->comb = (unsigned char)comb;
        int got = 0;
        while (i < len && !is_ws(s[i]) && s[i]!='>' && s[i]!='+' && s[i]!='~') {
            if (s[i] == '.' || s[i] == '#') {
                char kind = s[i]; i++;
                size_t vs = i;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~' && s[i]!=':' && s[i]!='[') i++;
                if (i > vs) {
                    if (kind == '.') { cpy_lower(pt->klass, sizeof pt->klass, s + vs, i - vs);
                                       out->spec += 10; }
                    else             { cpy_lower(pt->id, sizeof pt->id, s + vs, i - vs);
                                       out->spec += 100; }
                    got = 1;
                }
            } else if (s[i] == ':' || s[i] == '[') {
                char lead = s[i];
                i++;
                int dbl = 0;
                if (i < len && s[i] == ':') { i++; dbl = 1; }   /* ::before */
                size_t vs = i;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~' && s[i]!='(' &&
                       s[i]!=':' && s[i]!='[') i++;
                size_t vn = i - vs;
                /* A FUNCTIONAL pseudo: take its balanced parentheses with it.
                 * The scan above stops at whitespace, and the argument list is
                 * full of it -- `:where(.a, .b)` used to leave `.b)` behind to
                 * be read as another compound entirely. Consuming the parens is
                 * required whether or not the pseudo's meaning is implemented. */
                size_t args = 0, argn = 0;
                if (i < len && s[i] == '(') {
                    size_t d = 0;
                    args = i + 1;
                    do {
                        if (s[i] == '(') d++;
                        else if (s[i] == ')') d--;
                        i++;
                    } while (i < len && d);
                    argn = (i > args) ? (i - args - 1) : 0;   /* inside the parens */
                }
                if (lead == ':') {
                    char name[24];
                    cpy_lower(name, sizeof name, s + vs, vn);
                    if (argn && (!strcmp(name, "is") || !strcmp(name, "where") ||
                                 !strcmp(name, "matches") || !strcmp(name, "any"))) {
                        /* Only the FIRST such group is kept. `:is(a,b):is(c,d)`
                         * is a conjunction and merging would turn it into a
                         * disjunction, so the second is skipped -- looser, and
                         * loose is the safe direction here. */
                        unsigned short first, ms;
                        int n2 = parse_fn_args(s + args, argn, &first, &ms);
                        if (n2 && !pt->fn_any_n) {
                            pt->fn_any_first = first;
                            pt->fn_any_n = (unsigned char)(n2 > 255 ? 255 : n2);
                            /* :where() contributes NOTHING to specificity --
                             * that is the entire reason it exists and why
                             * design systems are written with it. :is() takes
                             * the strongest of its arguments. */
                            if (strcmp(name, "where")) out->spec += ms;
                        }
                        got = 1;
                    } else if (argn && !strcmp(name, "not")) {
                        unsigned short first, ms;
                        int n2 = parse_fn_args(s + args, argn, &first, &ms);
                        if (n2) {
                            /* Repeated :not()s MERGE, which is exact rather
                             * than approximate: `:not(a):not(b)` asks the same
                             * question as `:not(a, b)`. They are only mergeable
                             * because the pool hands out consecutive slots. */
                            if (!pt->fn_none_n) pt->fn_none_first = first;
                            int tot = pt->fn_none_n + n2;
                            pt->fn_none_n = (unsigned char)(tot > 255 ? 255 : tot);
                            out->spec += ms;
                        }
                        got = 1;
                    }
                    else if (!strcmp(name, "first-child")) { pt->first_child = 1; out->spec += 10; got = 1; }
                    else if (!strcmp(name, "last-child")) { pt->last_child = 1; out->spec += 10; got = 1; }
                    /* A PSEUDO-ELEMENT names a box the document does not
                     * contain, so the rule must not reach the element it hangs
                     * off. `::` always means one; the four legacy ones are also
                     * spelled with a single colon and have to be recognised by
                     * name. See css_sel.pseudo_elem. */
                    else if (dbl ||
                             !strcmp(name, "before") || !strcmp(name, "after") ||
                             !strcmp(name, "first-line") || !strcmp(name, "first-letter")) {
                        out->pseudo_elem = 1; got = 1;
                    }
                    /* anything else: skip the token and keep the compound --
                     * `a:hover` still styling `a` beats dropping the rule */
                }
            } else if (s[i] == '*') {
                i++; got = 1;                       /* any element, spec 0 */
            } else {
                size_t vs = i;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~' && s[i]!=':' && s[i]!='[') i++;
                if (i > vs) {
                    cpy_lower(pt->tag, sizeof pt->tag, s + vs, i - vs);
                    out->spec += 1;
                    got = 1;
                }
            }
        }
        if (got) nscratch++;
    }

    /* Keep the tail: the subject and as many of its nearest ancestors as fit.
     * Specificity was accumulated over EVERY compound above, including the
     * dropped ones, which is what a real engine reports. */
    int keep = nscratch < CSS_SEL_PARTS ? nscratch : CSS_SEL_PARTS;
    int from = nscratch - keep;
    for (int k = 0; k < keep; k++) out->part[k] = scratch[from + k];
    if (from > 0) out->part[0].comb = CSS_COMB_DESC;
    out->n = (unsigned char)keep;
    return out->n ? 0 : -1;
}

/* class="a b c" -- match one name against the whitespace-separated list */
static int has_class(const char *list, const char *want) {
    if (!list || !want || !*want) return 0;
    size_t wl = strlen(want);
    for (const char *p = list; *p; ) {
        while (*p && is_ws(*p)) p++;
        const char *s = p;
        while (*p && !is_ws(*p)) p++;
        size_t n = (size_t)(p - s);
        if (n == wl) {
            size_t k = 0;
            while (k < n && ci(s[k]) == want[k]) k++;
            if (k == n) return 1;
        }
    }
    return 0;
}

/* The previous ELEMENT sibling, or -1. The DOM stores forward links only, so
 * this is a walk from the parent -- which is fine: sibling combinators are
 * rare and the lists are short. */
static int prev_elem_sibling(struct html_doc *d, int n) {
    int p = d->nodes[n].parent;
    if (p < 0) return -1;
    int prev = -1;
    for (int c = d->nodes[p].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        if (c == n) return prev;
        if (d->nodes[c].kind == HTML_ELEM) prev = c;
    }
    return -1;
}

static int is_first_elem_child(struct html_doc *d, int n) {
    return prev_elem_sibling(d, n) < 0;
}

static int is_last_elem_child(struct html_doc *d, int n) {
    for (int c = d->nodes[n].next_sibling; c >= 0; c = d->nodes[c].next_sibling)
        if (d->nodes[c].kind == HTML_ELEM) return 0;
    return 1;
}

/* One :is()/:where()/:not() alternative against an element. Tag, class and id
 * only -- see parse_fn_args for what an alternative keeps. */
static int arg_matches(const struct css_fn_arg *a, const struct html_node *e) {
    if (a->tag[0]) {
        size_t i = 0;
        while (a->tag[i] && e->tag[i] && ci(e->tag[i]) == a->tag[i]) i++;
        if (a->tag[i] || e->tag[i]) return 0;
    }
    if (a->klass[0] && !has_class(e->klass, a->klass)) return 0;
    if (a->id[0]) {
        if (!e->id) return 0;
        size_t i = 0;
        while (a->id[i] && e->id[i] && ci(e->id[i]) == a->id[i]) i++;
        if (a->id[i] || e->id[i]) return 0;
    }
    /* A bare `*` alternative stores nothing and matches everything. */
    return 1;
}

static int part_matches(const struct css_sel_part *pt, struct html_doc *d, int n) {
    const struct html_node *e = &d->nodes[n];
    if (e->kind != HTML_ELEM) return 0;
    if (pt->first_child && !is_first_elem_child(d, n)) return 0;
    if (pt->last_child  && !is_last_elem_child(d, n))  return 0;
    if (pt->tag[0]) {
        size_t i = 0;
        while (pt->tag[i] && e->tag[i] && ci(e->tag[i]) == pt->tag[i]) i++;
        if (pt->tag[i] || e->tag[i]) return 0;
    }
    if (pt->klass[0] && !has_class(e->klass, pt->klass)) return 0;
    if (pt->id[0]) {
        if (!e->id) return 0;
        size_t i = 0;
        while (pt->id[i] && e->id[i] && ci(e->id[i]) == pt->id[i]) i++;
        if (pt->id[i] || e->id[i]) return 0;
    }
    /* :is()/:where() -- one of them must match. :not() -- none may. */
    if (pt->fn_any_n) {
        int hit = 0;
        for (int k = 0; k < pt->fn_any_n && !hit; k++)
            hit = arg_matches(&g_fn[pt->fn_any_first + k], e);
        if (!hit) return 0;
    }
    for (int k = 0; k < pt->fn_none_n; k++)
        if (arg_matches(&g_fn[pt->fn_none_first + k], e)) return 0;
    return 1;
}

int css_sel_match(const struct css_sel *sel, struct html_doc *d, int node) {
    if (!sel || !d || sel->n == 0 || node < 0 || node >= d->n) return 0;
    /* A pseudo-element's box is not in the document, so nothing here is it.
     * Until generated content exists, the honest answer is "no match" -- see
     * css_sel.pseudo_elem for what pretending otherwise costs. */
    if (sel->pseudo_elem) return 0;

    /* The SUBJECT (last part) must match the element itself. */
    int k = sel->n - 1;
    if (!part_matches(&sel->part[k], d, node)) return 0;

    /* Then the remaining parts, right to left, each reached by ITS OWN
     * combinator -- the one recorded on the part that follows it.
     *
     * Descendant and general-sibling are greedy: they take the first candidate
     * that matches and never back up. That is not strictly correct for
     * pathological selectors ("a a b" against nested a's) and it is right for
     * every selector a real document uses, while being unable to loop. */
    int cur = node;
    while (k > 0) {
        int comb = sel->part[k].comb;
        const struct css_sel_part *want = &sel->part[k - 1];
        if (comb == CSS_COMB_CHILD) {
            cur = d->nodes[cur].parent;
            if (cur < 0 || !part_matches(want, d, cur)) return 0;
        } else if (comb == CSS_COMB_ADJ) {
            cur = prev_elem_sibling(d, cur);
            if (cur < 0 || !part_matches(want, d, cur)) return 0;
        } else if (comb == CSS_COMB_SIB) {
            int p = prev_elem_sibling(d, cur);
            while (p >= 0 && !part_matches(want, d, p)) p = prev_elem_sibling(d, p);
            if (p < 0) return 0;
            cur = p;
        } else {
            int a = d->nodes[cur].parent;
            while (a >= 0 && !part_matches(want, d, a)) a = d->nodes[a].parent;
            if (a < 0) return 0;
            cur = a;
        }
        k--;
    }
    return 1;
}
