/* user/web/css/sheet.c -- the CASCADE.
 *
 * Parse a stylesheet into rules, then for one element decide which rules
 * apply and in what ORDER. Order is the whole point: the cascade is not "find
 * the winning rule", it is "apply every matching rule weakest-first and let
 * the strongest land last", because different rules contribute different
 * properties and they all have to survive.
 *
 * Sorting is by (specificity, document order) -- CSS's own tie-break, and the
 * reason two rules with the same selector strength resolve by which came
 * later rather than by luck.
 *
 * The rule table is FIXED. A stylesheet that overflows it loses its tail and
 * sets `truncated`, the same bargain the DOM arena makes: bounded appetite for
 * bytes that came from a stranger, and honesty about what was dropped.
 */
#include <string.h>

#include "html.h"
#include "css.h"

/* Skip /* ... *\/ comments and whitespace. */
static size_t skip_junk(const char *s, size_t len, size_t i) {
    for (;;) {
        while (i < len && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'||s[i]=='\f')) i++;
        if (i + 1 < len && s[i]=='/' && s[i+1]=='*') {
            i += 2;
            while (i + 1 < len && !(s[i]=='*' && s[i+1]=='/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        return i;
    }
}

/* Append `text`'s rules to `sheet`, continuing the source-order counter.
 * Split out from css_sheet_parse so a matching @media block can be parsed
 * IN PLACE -- its rules keep their position in the cascade, which is what
 * makes a media override actually beat the base rule it overrides. */
static unsigned short css_sheet_parse_into(struct css_sheet *sheet, const char *text,
                                           size_t len, unsigned short order);

void css_sheet_parse(struct css_sheet *sheet, const char *text, size_t len) {
    if (!sheet) return;
    memset(sheet, 0, sizeof *sheet);
    css_vars_reset();
    if (!text || !len) return;
    /* Collect custom properties across the WHOLE sheet first, so a rule can
     * use a var defined below it -- which CSS allows and a resolve-as-you-go
     * pass would get wrong. (Inside @media too: a var behind a query the page
     * does not match is a corner this does not chase.) */
    css_vars_collect(text, len);
    css_sheet_parse_into(sheet, text, len, 0);
}

static unsigned short css_sheet_parse_into(struct css_sheet *sheet, const char *text,
                                           size_t len, unsigned short order) {
    size_t i = 0;
    while (i < len) {
        i = skip_junk(text, len, i);
        if (i >= len) break;

        /* An at-rule. @media is EVALUATED -- a mobile-first sheet keeps its
         * desktop rules behind one, and skipping them renders the phone
         * layout at desktop size. Everything else (@import, @font-face,
         * @keyframes, @supports) is still skipped whole: we cannot honour the
         * condition, and a wrongly-applied block rewrites the page. */
        if (text[i] == '@') {
            size_t kw_s = ++i;
            while (i < len && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' &&
                   text[i] != '\r' && text[i] != '{' && text[i] != ';') i++;
            int is_media = (i - kw_s == 5) &&
                           (text[kw_s]=='m'||text[kw_s]=='M') &&
                           !strncmp(text + kw_s + 1, "edia", 4);
            size_t q_s = i;
            while (i < len && text[i] != '{' && text[i] != ';') i++;
            if (i >= len || text[i] == ';') { if (i < len) i++; continue; }
            size_t q_e = i;
            i++;                                   /* past '{' */
            size_t body_s = i;
            int depth = 1;
            while (i < len && depth) {
                if (text[i] == '{') depth++;
                else if (text[i] == '}') { depth--; if (!depth) break; }
                i++;
            }
            size_t body_e = i;
            if (i < len) i++;                      /* past the closing '}' */
            /* A matching block's rules are parsed INTO THIS SHEET, in place,
             * so they keep their source position -- which is what makes a
             * media override beat the base rule it is overriding. */
            if (is_media && css_media_matches(text + q_s, q_e - q_s))
                order = css_sheet_parse_into(sheet, text + body_s, body_e - body_s, order);
            continue;
        }

        /* selector list, up to '{' */
        size_t ss = i;
        while (i < len && text[i] != '{') i++;
        if (i >= len) break;
        size_t se = i;
        i++;                                        /* past '{' */

        /* declaration block, to the matching '}' */
        size_t ds = i;
        while (i < len && text[i] != '}') i++;
        size_t de = i;
        if (i < len) i++;                           /* past '}' */

        /* one rule per comma-separated selector: they share declarations but
         * each carries its OWN specificity, which is why they cannot be one
         * rule with a list inside */
        size_t p = ss;
        while (p < se) {
            size_t cs = p;
            while (p < se && text[p] != ',') p++;
            size_t ce = p;
            if (p < se) p++;
            while (cs < ce && (text[cs]==' '||text[cs]=='\t'||text[cs]=='\n'||text[cs]=='\r')) cs++;
            while (ce > cs && (text[ce-1]==' '||text[ce-1]=='\t'||text[ce-1]=='\n'||text[ce-1]=='\r')) ce--;
            if (ce <= cs) continue;

            if (sheet->n >= CSS_MAX_RULES) { sheet->truncated = 1; return order; }
            struct css_rule *r = &sheet->rules[sheet->n];
            if (css_sel_parse(text + cs, ce - cs, &r->sel) != 0) continue;
            r->decls = text + ds;
            r->decls_len = de - ds;
            r->order = order++;
            sheet->n++;
        }
    }
    return order;
}

void css_sheet_apply(const struct css_sheet *sheet, struct html_doc *doc,
                     int node, struct vstyle *out) {
    if (!sheet || !doc || !out || node < 0 || node >= doc->n) return;

    /* Collect the matches, then apply in cascade order. Two passes rather than
     * one because "weakest first" is not the order they are found in.
     *
     * The index type has to HOLD a rule index. It was `unsigned char`, which
     * was exactly wide enough while CSS_MAX_RULES was 256 and became silent
     * corruption the moment the cap was raised: rule 300 was stored as 44, so
     * the cascade applied whichever rule happened to live at the truncated
     * index. lobste.rs came out completely blank because a rule for something
     * else landed on <body> as display:none.
     *
     * Static rather than automatic: 4096 entries is 8KB, this is called once
     * per element per frame, and it does not recurse. */
    static unsigned short idx[CSS_MAX_RULES];
    int m = 0;
    for (int i = 0; i < sheet->n && m < CSS_MAX_RULES; i++)
        if (css_sel_match(&sheet->rules[i].sel, doc, node)) idx[m++] = (unsigned short)i;
    if (!m) return;

    /* insertion sort by (specificity, document order) -- ascending, so the
     * strongest rule is applied LAST and therefore wins each property it
     * sets, while weaker rules still contribute the properties it doesn't */
    for (int a = 1; a < m; a++) {
        unsigned short key = idx[a];
        const struct css_rule *rk = &sheet->rules[key];
        int b = a - 1;
        while (b >= 0) {
            const struct css_rule *rb = &sheet->rules[idx[b]];
            int heavier = rb->sel.spec > rk->sel.spec ||
                          (rb->sel.spec == rk->sel.spec && rb->order > rk->order);
            if (!heavier) break;
            idx[b + 1] = idx[b];
            b--;
        }
        idx[b + 1] = key;
    }

    for (int a = 0; a < m; a++)
        css_apply_decls(sheet->rules[idx[a]].decls, sheet->rules[idx[a]].decls_len, out);
}
