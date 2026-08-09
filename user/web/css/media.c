/* user/web/css/media.c -- evaluating a media query.
 *
 * `@media (min-width: 700px) { ... }`. Until this existed the whole block was
 * skipped, which was the safe direction while nothing could evaluate it -- a
 * wrongly-applied media block rewrites the entire page -- but it means a
 * mobile-first stylesheet loses every desktop rule it has. Most sites are
 * written that way now: the base rules are the phone layout and everything
 * else lives behind a min-width. Skipping them does not make such a page
 * slightly narrower, it renders the phone version at desktop size.
 *
 * The environment is set by the app (the window's content width, and whether
 * the desktop is dark) rather than assumed here, because the browser is not the
 * only thing that will want to ask.
 *
 * Supported: media types (`all`, `screen`, `print`, `speech`, with an optional
 * `only`), `and`, comma-separated alternatives, a leading `not`, and the
 * features that decide layout in practice -- min/max width and height,
 * orientation, and prefers-color-scheme. An UNRECOGNISED feature makes its
 * conjunction false, which is what CSS says and is also the safe direction:
 * a query we do not understand does not get to restyle the page.
 */
#include <string.h>

#include "css.h"

static float g_vw = 940.0f, g_vh = 620.0f;
static int   g_dark = 1;          /* this desktop is dark by default */

void css_media_set(float w, float h, int dark) {
    if (w > 0) g_vw = w;
    if (h > 0) g_vh = h;
    g_dark = dark ? 1 : 0;
}

float css_viewport_w(void) { return g_vw; }
float css_viewport_h(void) { return g_vh; }

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

/* token compare, case-insensitive */
static int teq(const char *s, size_t n, const char *w) {
    size_t i = 0;
    for (; i < n && w[i]; i++) if (ci(s[i]) != w[i]) return 0;
    return i == n && !w[i];
}

/* A length in px. em/rem are against the 15px body this renderer uses, which
 * is the same assumption decl.c makes -- one number, one place to change. */
static float len_px(const char *s, size_t n) {
    while (n && is_ws(*s)) { s++; n--; }
    float v = 0; size_t i = 0; int seen = 0, frac = 0; float scale = 1;
    for (; i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'); i++) {
        if (s[i] == '.') { frac = 1; continue; }
        if (frac) { scale *= 0.1f; v += (float)(s[i] - '0') * scale; }
        else v = v * 10.0f + (float)(s[i] - '0');
        seen = 1;
    }
    if (!seen) return -1.0f;
    if (i + 1 < n && (ci(s[i]) == 'e') && ci(s[i+1]) == 'm') return v * 15.0f;
    if (i + 2 < n && (ci(s[i]) == 'r') && ci(s[i+1]) == 'e' && ci(s[i+2]) == 'm') return v * 15.0f;
    return v;
}

/* teq with the ends trimmed -- a range's operands carry the spaces around the
 * operator. */
static int teq_trim(const char *s, size_t n, const char *w) {
    while (n && is_ws(*s)) { s++; n--; }
    while (n && is_ws(s[n-1])) n--;
    return teq(s, n, w);
}

/* RANGE SYNTAX: `(width <= 885px)`, `(width > 600px)`, and the two-sided
 * `(400px <= width < 700px)`.
 *
 * This is not an exotic corner. It is what every modern build tool now emits --
 * 224 of the media queries in Brave's stylesheet are written this way and only
 * a handful use min-width/max-width -- and a feature we do not recognise is
 * reported as NOT MATCHING, so the entire responsive layer of such a sheet
 * silently evaluated to false. At a wide viewport that happens to be right for
 * `width <= 885px` and wrong for everything else, which is the worst kind of
 * wrong: correct on the cases you check first.
 *
 * Returns 1/0 for a match, or -1 if this is not a range test at all. */
static int range_matches(const char *s, size_t n) {
    /* Find the comparison operators. A range has one or two. */
    int op1 = -1, op2 = -1;          /* index of the first char of each */
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '<' || s[i] == '>') {
            if (op1 < 0) op1 = (int)i;
            else if (op2 < 0) op2 = (int)i;
        }
    }
    if (op1 < 0) return -1;

    /* op text -> a comparison against the viewport. `lhs OP rhs`. */
    struct { int lt, eq; } o1 = { s[op1] == '<', (size_t)op1 + 1 < n && s[op1+1] == '=' };
    const char *a = s; size_t an = (size_t)op1;
    const char *b = s + op1 + 1 + (o1.eq ? 1 : 0);
    size_t bn = n - (size_t)(b - s);
    if (op2 > 0) bn = (size_t)(s + op2 - b);

    /* Which side names the feature? `width <= 885px` or `885px <= width`. */
    int axis = 0;                    /* 1 = width, 2 = height */
    const char *lenp; size_t lenn;
    int feature_left;
    if (teq_trim(a, an, "width"))       { axis = 1; feature_left = 1; lenp = b; lenn = bn; }
    else if (teq_trim(a, an, "height")) { axis = 2; feature_left = 1; lenp = b; lenn = bn; }
    else if (teq_trim(b, bn, "width"))  { axis = 1; feature_left = 0; lenp = a; lenn = an; }
    else if (teq_trim(b, bn, "height")) { axis = 2; feature_left = 0; lenp = a; lenn = an; }
    else return -1;

    float v = len_px(lenp, lenn);
    if (v < 0) return -1;
    float actual = (axis == 1) ? g_vw : g_vh;
    /* Normalise to `actual OP v`: if the feature was on the right, the
     * comparison reads the other way round. */
    int lt = o1.lt, eq = o1.eq;
    if (!feature_left) lt = !lt;
    int first = lt ? (eq ? actual <= v : actual < v)
                   : (eq ? actual >= v : actual > v);
    if (op2 < 0) return first ? 1 : 0;
    if (!first) return 0;

    /* The second half of a two-sided range: `lo <= width <= hi`. The feature is
     * in the middle, so this comparison always reads feature-on-the-left. */
    int lt2 = s[op2] == '<', eq2 = (size_t)op2 + 1 < n && s[op2+1] == '=';
    const char *c2 = s + op2 + 1 + (eq2 ? 1 : 0);
    float v2 = len_px(c2, n - (size_t)(c2 - s));
    if (v2 < 0) return 0;
    return lt2 ? (eq2 ? actual <= v2 : actual < v2)
               : (eq2 ? actual >= v2 : actual > v2);
}

/* One `(feature: value)` test, or a bare `(feature)`. */
static int feature_matches(const char *s, size_t n) {
    while (n && is_ws(*s)) { s++; n--; }
    while (n && is_ws(s[n-1])) n--;
    { int r = range_matches(s, n); if (r >= 0) return r; }
    size_t c = 0;
    while (c < n && s[c] != ':') c++;
    const char *name = s; size_t nn = c;
    while (nn && is_ws(name[nn-1])) nn--;
    const char *val = (c < n) ? s + c + 1 : 0;
    size_t vn = (c < n) ? n - c - 1 : 0;

    if (teq(name, nn, "min-width"))  return val && len_px(val, vn) <= g_vw;
    if (teq(name, nn, "max-width"))  return val && len_px(val, vn) >= g_vw;
    if (teq(name, nn, "width"))      return val && len_px(val, vn) == g_vw;
    if (teq(name, nn, "min-height")) return val && len_px(val, vn) <= g_vh;
    if (teq(name, nn, "max-height")) return val && len_px(val, vn) >= g_vh;
    if (teq(name, nn, "orientation")) {
        if (!val) return 0;
        while (vn && is_ws(*val)) { val++; vn--; }
        while (vn && is_ws(val[vn-1])) vn--;
        return teq(val, vn, g_vw >= g_vh ? "landscape" : "portrait");
    }
    if (teq(name, nn, "prefers-color-scheme")) {
        if (!val) return 0;
        while (vn && is_ws(*val)) { val++; vn--; }
        while (vn && is_ws(val[vn-1])) vn--;
        return teq(val, vn, g_dark ? "dark" : "light");
    }
    /* THIS MACHINE HAS A POINTER. `(hover: hover)` and `(pointer: fine)` are
     * how a page asks whether it is on a desktop, and 42 of Brave's queries
     * do. Answering "no" to both -- which is what an unknown feature does --
     * gave every one of them the touch layout. */
    if (teq(name, nn, "hover"))   return val && teq_trim(val, vn, "hover");
    if (teq(name, nn, "any-hover")) return val && teq_trim(val, vn, "hover");
    if (teq(name, nn, "pointer") || teq(name, nn, "any-pointer"))
        return val && teq_trim(val, vn, "fine");
    /* A feature we do not know cannot be claimed to hold. */
    return 0;
}

/* One conjunction: `screen and (min-width: 700px) and (orientation: landscape)`,
 * optionally led by `not`. */
static int conjunction_matches(const char *s, size_t n) {
    int negate = 0, result = 1, any = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && is_ws(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '(') {
            size_t depth = 1, j = i + 1;
            while (j < n && depth) {
                if (s[j] == '(') depth++;
                else if (s[j] == ')') depth--;
                if (depth) j++;
            }
            if (!feature_matches(s + i + 1, j - i - 1)) result = 0;
            any = 1;
            i = j < n ? j + 1 : n;
            continue;
        }
        size_t ts = i;
        while (i < n && !is_ws(s[i]) && s[i] != '(') i++;
        size_t tn = i - ts;
        if (!tn) continue;
        if      (teq(s + ts, tn, "not"))  negate = 1;
        else if (teq(s + ts, tn, "and"))  ;               /* joins, no-op */
        else if (teq(s + ts, tn, "only")) ;               /* legacy guard  */
        else if (teq(s + ts, tn, "all") || teq(s + ts, tn, "screen")) { any = 1; }
        else if (teq(s + ts, tn, "print") || teq(s + ts, tn, "speech")) {
            /* We are a screen. A print-only block must not restyle the page. */
            result = 0; any = 1;
        } else {
            /* an unknown media type */
            result = 0; any = 1;
        }
    }
    if (!any) return 0;                     /* an empty query matches nothing */
    return negate ? !result : result;
}

int css_media_matches(const char *q, size_t n) {
    if (!q) return 0;
    while (n && is_ws(*q)) { q++; n--; }
    while (n && is_ws(q[n-1])) n--;
    /* `@media { ... }` with no query at all is `all` -- rare, but a page that
     * writes it means "always". */
    if (!n) return 1;
    /* comma-separated alternatives: any one of them is enough */
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || q[i] == ',') {
            if (conjunction_matches(q + start, i - start)) return 1;
            start = i + 1;
        }
    }
    return 0;
}
