/* user/web/css/decl.c -- what a declaration MEANS.
 *
 * "color: #c00; font-weight: bold" -> fields in a struct vstyle. That is the
 * whole job: no selectors, no cascade, no tree. It is shared verbatim by
 * inline style="" and by rules in a stylesheet, which is why it is its own
 * file -- the two callers arrive from opposite directions and must agree
 * perfectly about what a value means.
 *
 * Every property here is one this renderer can actually honour. A parser that
 * accepts `float: left` and then ignores it has not implemented floats; it has
 * implemented a lie that is harder to find than a missing feature.
 */
#include <string.h>
#include <stdlib.h>

#include "html.h"
#include "css.h"

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int tok_eq(const char *s, size_t n, const char *w) {
    size_t wl = strlen(w);
    if (n != wl) return 0;
    for (size_t i = 0; i < n; i++) if (ci(s[i]) != w[i]) return 0;
    return 1;
}
static int tok_has(const char *s, size_t n, const char *w) {   /* substring */
    size_t wl = strlen(w);
    if (wl > n) return 0;
    for (size_t i = 0; i + wl <= n; i++) {
        size_t j = 0;
        while (j < wl && ci(s[i+j]) == w[j]) j++;
        if (j == wl) return 1;
    }
    return 0;
}

/* A length in px. "12px", "12", "1.5em" (em ~ 16px, close enough for a
 * document). Returns 0 and sets *ok=0 for things we cannot honour, so the
 * caller can leave the property alone rather than write a wrong number. */
/* A percentage, 0-100, or -1 if the value is not one. Kept separate from
 * len_px because a percentage is not a length until a containing block exists;
 * conflating them is how `width: 50%` quietly became `width: 0`. */
static int len_pct(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i]==' '||s[i]=='\t')) i++;
    double v = 0; int seen = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; seen = 1; }
    if (i < n && s[i] == '.') {
        i++; double f = 0.1;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i]-'0') * f; f *= 0.1; i++; }
    }
    while (i < n && (s[i]==' '||s[i]=='\t')) i++;
    if (!seen || i >= n || s[i] != '%') return -1;
    return v > 100.0 ? 100 : (int)(v + 0.5);
}

static short len_px(const char *s, size_t n, int *ok) {
    *ok = 0;
    size_t i = 0;
    int neg = 0;
    while (i < n && (s[i]==' '||s[i]=='\t')) i++;
    if (i < n && (s[i]=='-'||s[i]=='+')) { neg = (s[i]=='-'); i++; }
    if (i >= n || s[i] < '0' || s[i] > '9') {
        if (tok_has(s, n, "auto")) { *ok = 1; return 0; }
        return 0;
    }
    double v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') {
        i++; double f = 0.1;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i]-'0') * f; f *= 0.1; i++; }
    }
    if (tok_has(s + i, n - i, "em") || tok_has(s + i, n - i, "rem")) v *= 16.0;
    else if (tok_has(s + i, n - i, "vw")) v = v * (double)css_viewport_w() / 100.0;
    else if (tok_has(s + i, n - i, "vh")) v = v * (double)css_viewport_h() / 100.0;
    else if (tok_has(s + i, n - i, "%")) { return 0; }   /* percentages need a container */
    *ok = 1;
    /* Generous, because this same parser serves margins (tens of px) and
     * image widths (hundreds). The ceiling exists to stop a hostile stylesheet
     * from asking for a mile, not to express a design opinion. */
    if (v > 4000) v = 4000;
    return (short)(neg ? -v : v);
}

/* A colour. #rgb, #rrggbb, and the handful of names a document actually uses.
 * Returns 0 if unrecognised -- vstyle treats 0 as "no author colour". */
static unsigned css_color(const char *s, size_t n) {
    while (n && (*s==' '||*s=='\t')) { s++; n--; }
    while (n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]==';')) n--;
    if (n && *s == '#') {
        unsigned v = 0; size_t d = 0;
        for (size_t i = 1; i < n; i++) {
            int c = ci(s[i]), h;
            if (c >= '0' && c <= '9') h = c - '0';
            else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
            else break;
            v = (v << 4) | (unsigned)h; d++;
        }
        if (d == 3) {   /* #rgb -> #rrggbb */
            unsigned r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            v = (r * 17u << 16) | (g * 17u << 8) | (b * 17u);
            d = 6;
        }
        if (d == 6) return 0xFF000000u | v;
        return 0;
    }
    /* rgb()/rgba(). Modern stylesheets write colours this way constantly, and
     * a page whose every colour is unparsed renders as if it had no CSS. The
     * alpha of rgba() is honoured only as "transparent or not": this renderer
     * composites a background as a solid, and a half-transparent one would
     * need the box behind it, which the box model does not expose. */
    if (n > 4 && (s[0]=='r'||s[0]=='R') && (s[1]=='g'||s[1]=='G') && (s[2]=='b'||s[2]=='B')) {
        size_t i = 3;
        if (i < n && (s[i]=='a'||s[i]=='A')) i++;
        while (i < n && (s[i]==' '||s[i]=='\t')) i++;
        if (i < n && s[i] == '(') {
            i++;
            int comp[4] = {0,0,0,255}, nc = 0, frac = 0;
            while (i < n && nc < 4) {
                while (i < n && (s[i]==' '||s[i]=='\t'||s[i]==','||s[i]=='/')) i++;
                if (i >= n || s[i] == ')') break;
                int val = 0, digits = 0, isfrac = 0, fd = 0;
                while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '%')) {
                    if (s[i] == '.') { isfrac = 1; i++; continue; }
                    if (s[i] == '%') { i++; break; }
                    if (isfrac) { if (fd == 0) { frac = s[i] - '0'; fd = 1; } }
                    else { val = val * 10 + (s[i] - '0'); digits++; }
                    i++;
                    if (digits > 4) break;
                }
                if (nc == 3) {                 /* alpha: 0 / 0.x / 1 */
                    comp[3] = (val == 0 && !frac) ? 0 : 255;
                } else {
                    comp[nc] = val > 255 ? 255 : val;
                }
                nc++;
            }
            if (nc >= 3) {
                if (!comp[3]) return 0;        /* fully transparent = not set */
                return 0xFF000000u | ((unsigned)comp[0] << 16) |
                       ((unsigned)comp[1] << 8) | (unsigned)comp[2];
            }
        }
        return 0;
    }
    static const struct { const char *name; unsigned rgb; } named[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},
        {"blue",0x0000FF},{"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},
        {"maroon",0x800000},{"navy",0x000080},{"teal",0x008080},{"olive",0x808000},
        {"purple",0x800080},{"orange",0xFFA500},{"yellow",0xFFFF00},{"lime",0x00FF00},
        {"aqua",0x00FFFF},{"cyan",0x00FFFF},{"fuchsia",0xFF00FF},{"magenta",0xFF00FF},
    };
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (tok_eq(s, n, named[i].name)) return 0xFF000000u | named[i].rgb;
    return 0;
}

int css_apply_decls(const char *text, size_t len, struct vstyle *out) {
    if (!text || !out) return 0;
    int applied = 0;
    size_t i = 0;
    while (i < len) {
        /* property */
        while (i < len && (text[i]==' '||text[i]=='\t'||text[i]=='\n'||
                           text[i]=='\r'||text[i]==';')) i++;
        if (i >= len) break;
        size_t ps = i;
        while (i < len && text[i] != ':' && text[i] != ';' && text[i] != '}') i++;
        size_t pn = i - ps;
        while (pn && (text[ps+pn-1]==' '||text[ps+pn-1]=='\t')) pn--;
        if (i >= len || text[i] != ':') {            /* junk: skip to the ';' */
            while (i < len && text[i] != ';') i++;
            continue;
        }
        i++;                                          /* past ':' */
        /* value */
        while (i < len && (text[i]==' '||text[i]=='\t')) i++;
        size_t vs = i;
        while (i < len && text[i] != ';' && text[i] != '}') i++;
        size_t vn = i - vs;
        while (vn && (text[vs+vn-1]==' '||text[vs+vn-1]=='\t'||
                      text[vs+vn-1]=='\n'||text[vs+vn-1]=='\r')) vn--;
        const char *p = text + ps, *v = text + vs;
        if (!pn || !vn) continue;

        /* A custom property DEFINES rather than sets: record it and move on.
         * (The sheet already collected these at parse time; an inline
         * style="--x: y" reaches us only here.) */
        if (pn > 2 && p[0] == '-' && p[1] == '-') {
            css_var_set(p, pn, v, vn);
            applied++;
            continue;
        }
        /* ...and every other value may USE one. Substituting before the value
         * is interpreted means every property below gets var() support for
         * free, instead of each one having to remember. */
        char expanded[160];
        if (css_var_expand(v, vn, expanded, sizeof expanded)) {
            v = expanded;
            vn = strlen(expanded);
        }

        int ok = 0;
        if (tok_eq(p, pn, "color")) {
            unsigned c = css_color(v, vn);
            if (c) { out->color = c; out->color_own = 1; ok = 1; }
        } else if (tok_eq(p, pn, "font-weight")) {
            if (tok_eq(v, vn, "bold") || tok_eq(v, vn, "bolder")) { out->bold = 1; ok = 1; }
            else if (tok_eq(v, vn, "normal") || tok_eq(v, vn, "lighter")) { out->bold = 0; ok = 1; }
            else { int n2 = atoi(v); if (n2 >= 100 && n2 <= 900) { out->bold = n2 >= 600; ok = 1; } }
        } else if (tok_eq(p, pn, "font-style")) {
            if (tok_eq(v, vn, "italic") || tok_eq(v, vn, "oblique")) { out->italic = 1; ok = 1; }
            else if (tok_eq(v, vn, "normal")) { out->italic = 0; ok = 1; }
        } else if (tok_eq(p, pn, "font-family")) {
            /* the only family distinction this renderer HAS is mono vs not */
            if (tok_has(v, vn, "mono") || tok_has(v, vn, "courier") ||
                tok_has(v, vn, "consol")) { out->mono = 1; ok = 1; }
            else { out->mono = 0; ok = 1; }
        } else if (tok_eq(p, pn, "font-size")) {
            /* mapped onto the four roles the toolkit has, by px threshold --
             * an honest approximation, not a pretend continuum */
            int lok = 0; short px = len_px(v, vn, &lok);
            if (tok_eq(v, vn, "small") || tok_eq(v, vn, "x-small")) { out->size = 1; ok = 1; }
            else if (tok_eq(v, vn, "large") || tok_eq(v, vn, "x-large")) { out->size = 2; ok = 1; }
            else if (tok_eq(v, vn, "xx-large")) { out->size = 3; ok = 1; }
            else if (lok && px > 0) {
                out->size = px >= 24 ? 3 : px >= 19 ? 2 : px <= 13 ? 1 : 0;
                ok = 1;
            }
        } else if (tok_eq(p, pn, "text-decoration") || tok_eq(p, pn, "text-decoration-line")) {
            if (tok_has(v, vn, "underline")) { out->underline = 1; ok = 1; }
            else if (tok_has(v, vn, "none")) { out->underline = 0; ok = 1; }
        } else if (tok_eq(p, pn, "display")) {
            if (tok_eq(v, vn, "none"))        { out->display = VD_NONE;      ok = 1; }
            else if (tok_eq(v, vn, "block"))  { out->display = VD_BLOCK;     ok = 1; }
            else if (tok_eq(v, vn, "inline")) { out->display = VD_INLINE;    ok = 1; }
            else if (tok_eq(v, vn, "list-item")) { out->display = VD_LIST_ITEM; ok = 1; }
            else if (tok_eq(v, vn, "inline-block")) { out->display = VD_INLINE; ok = 1; }
            else if (tok_eq(v, vn, "flex") || tok_eq(v, vn, "inline-flex"))
                                             { out->display = VD_FLEX;  ok = 1; }
            else if (tok_eq(v, vn, "grid") || tok_eq(v, vn, "inline-grid"))
                                             { out->display = VD_GRID;  ok = 1; }
            /* table displays reach here from a stylesheet as well as from the
             * tag table, which is the point of naming them in the display
             * enum in the first place */
            else if (tok_eq(v, vn, "table"))      { out->display = VD_TABLE; ok = 1; }
            else if (tok_eq(v, vn, "table-row"))  { out->display = VD_ROW;   ok = 1; }
            else if (tok_eq(v, vn, "table-cell")) { out->display = VD_CELL;  ok = 1; }
        } else if (tok_eq(p, pn, "flex-direction")) {
            if (tok_eq(v, vn, "column") || tok_eq(v, vn, "column-reverse"))
                 { out->flex_col = 1; ok = 1; }
            else if (tok_eq(v, vn, "row") || tok_eq(v, vn, "row-reverse"))
                 { out->flex_col = 0; ok = 1; }
        } else if (tok_eq(p, pn, "flex-wrap")) {
            if (tok_eq(v, vn, "wrap") || tok_eq(v, vn, "wrap-reverse")) { out->flex_wrap = 1; ok = 1; }
            else if (tok_eq(v, vn, "nowrap")) { out->flex_wrap = 0; ok = 1; }
        } else if (tok_eq(p, pn, "justify-content")) {
            if      (tok_eq(v, vn, "center"))        { out->justify = VJ_CENTER;  ok = 1; }
            else if (tok_eq(v, vn, "flex-end") || tok_eq(v, vn, "end") ||
                     tok_eq(v, vn, "right"))         { out->justify = VJ_END;     ok = 1; }
            else if (tok_eq(v, vn, "space-between")) { out->justify = VJ_BETWEEN; ok = 1; }
            else if (tok_eq(v, vn, "flex-start") || tok_eq(v, vn, "start") ||
                     tok_eq(v, vn, "left"))          { out->justify = VJ_START;   ok = 1; }
            /* space-around / space-evenly fall back to space-between: the gap
             * is in the wrong place but the items are still spread, which is
             * nearer the author's intent than piling them at the start. */
            else if (tok_eq(v, vn, "space-around") || tok_eq(v, vn, "space-evenly"))
                                                     { out->justify = VJ_BETWEEN; ok = 1; }
        } else if (tok_eq(p, pn, "align-items")) {
            if      (tok_eq(v, vn, "center"))     { out->align_items = VJ_CENTER;  ok = 1; }
            else if (tok_eq(v, vn, "flex-end") || tok_eq(v, vn, "end"))
                                                  { out->align_items = VJ_END;     ok = 1; }
            else if (tok_eq(v, vn, "stretch"))    { out->align_items = VJ_STRETCH; ok = 1; }
            else if (tok_eq(v, vn, "flex-start") || tok_eq(v, vn, "start") ||
                     tok_eq(v, vn, "baseline"))   { out->align_items = VJ_START;   ok = 1; }
        } else if (tok_eq(p, pn, "gap") || tok_eq(p, pn, "row-gap") ||
                   tok_eq(p, pn, "column-gap") || tok_eq(p, pn, "grid-gap")) {
            /* `gap: 12px 20px` states row then column; this layout has one
             * spacing, so the first (row) wins -- the axis a stacked list
             * actually notices. */
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok && px >= 0) { out->gap = px; ok = 1; }
        } else if (tok_eq(p, pn, "flex") || tok_eq(p, pn, "flex-grow")) {
            /* `flex: 1`, `flex: 1 1 auto`, `flex-grow: 2` -- what matters here
             * is whether this child takes the leftover space at all. */
            int nonzero = 0;
            for (size_t k = 0; k < vn; k++) {
                if (v[k] >= '1' && v[k] <= '9') { nonzero = 1; break; }
                if (v[k] == ' ') break;
            }
            if (tok_eq(v, vn, "none")) { out->grow = 0; ok = 1; }
            else { out->grow = nonzero ? 1 : 0; ok = 1; }
        } else if (tok_eq(p, pn, "grid-template-columns")) {
            /* Track SIZES, not just a count. `12.25rem minmax(0,1fr)` is a
             * 196px sidebar beside everything else, and reading it as "two
             * columns" and sizing both from their content is how a page's
             * chrome ends up divided by how much text each side happens to
             * hold.
             *
             * What each form becomes:
             *   200px, 12.25rem, 3em  -> a fixed track
             *   1fr, 2fr             -> a weighted track
             *   minmax(a, b)         -> b, the maximum: that is the size it
             *                           takes whenever there is room, and
             *                           there usually is
             *   auto, min/max-content, anything unrecognised -> content-sized,
             *                           which is the old behaviour and a safe
             *                           place to land
             *   repeat(n, <one>)     -> n copies of it
             */
            unsigned char tm[VSTYLE_TRACKS];
            short tv[VSTYLE_TRACKS];
            int nt = 0, cols = 0;
            size_t k = 0;
            int rep = 1;
            if (vn > 7 && (v[0]=='r'||v[0]=='R') && !strncmp(v + 1, "epeat", 5)) {
                while (k < vn && v[k] != '(') k++;
                k++;
                rep = 0;
                while (k < vn && v[k] >= '0' && v[k] <= '9') { rep = rep * 10 + (v[k] - '0'); k++; }
                while (k < vn && (v[k] == ',' || v[k] == ' ')) k++;
                if (rep < 1) rep = 1;
            }
            /* one pass over the track list, honouring nesting so a comma
             * inside minmax() does not read as a track separator */
            while (k < vn) {
                while (k < vn && (v[k]==' '||v[k]=='\t')) k++;
                if (k >= vn) break;
                size_t ts = k; int depth = 0;
                while (k < vn && !((v[k]==' '||v[k]=='\t') && depth == 0)) {
                    if (v[k] == '(') depth++;
                    else if (v[k] == ')') { if (depth) depth--; else break; }
                    k++;
                }
                size_t tl = k - ts;
                if (!tl) break;
                const char *t = v + ts;
                /* minmax(a,b) -> b */
                if (tl > 7 && (t[0]=='m'||t[0]=='M') && !strncmp(t + 1, "inmax(", 6)) {
                    size_t c2 = 0;
                    while (c2 < tl && t[c2] != ',') c2++;
                    if (c2 < tl) { t += c2 + 1; tl -= c2 + 1; }
                    while (tl && (*t == ' ')) { t++; tl--; }
                    while (tl && (t[tl-1] == ')' || t[tl-1] == ' ')) tl--;
                }
                cols++;
                if (nt < VSTYLE_TRACKS) {
                    if (tl > 2 && (t[tl-2]=='f'||t[tl-2]=='F') && (t[tl-1]=='r'||t[tl-1]=='R')) {
                        float f = 0; size_t i2 = 0;
                        while (i2 < tl - 2 && t[i2] >= '0' && t[i2] <= '9') { f = f*10 + (t[i2]-'0'); i2++; }
                        if (f <= 0) f = 1;
                        tm[nt] = VT_FR; tv[nt] = (short)(f * 16.0f);
                    } else {
                        int lok = 0; short px = len_px(t, tl, &lok);
                        if (lok && px > 0) { tm[nt] = VT_PX; tv[nt] = px; }
                        else               { tm[nt] = VT_AUTO; tv[nt] = 0; }
                    }
                    nt++;
                }
            }
            if (rep > 1 && nt == 1) {           /* repeat(n, <one track>) */
                cols = rep;
                for (int i = 1; i < rep && i < VSTYLE_TRACKS; i++) { tm[i] = tm[0]; tv[i] = tv[0]; }
                nt = rep < VSTYLE_TRACKS ? rep : VSTYLE_TRACKS;
            }
            if (cols > 0 && cols <= 64) {
                out->grid_cols = (unsigned char)cols;
                out->grid_ntrack = (unsigned char)nt;
                for (int i = 0; i < nt; i++) { out->grid_track_mode[i] = tm[i]; out->grid_track_val[i] = tv[i]; }
                ok = 1;
            }
        } else if (tok_eq(p, pn, "width") || tok_eq(p, pn, "height")) {
            int is_w = tok_eq(p, pn, "width");
            short *dst_px  = is_w ? &out->width : &out->height;
            unsigned char *dst_pct = is_w ? &out->width_pct : &out->height_pct;
            float cpct = 0, cpx = 0;
            if (css_calc(v, vn, &cpct, &cpx) == 0) {
                /* a calc reduces to pct% + px; both halves travel */
                if (cpct != 0) { *dst_pct = (unsigned char)(cpct > 100 ? 100 : (cpct < 0 ? 0 : cpct));
                                 *dst_px  = (short)cpx; }
                else           { *dst_pct = 0; *dst_px = (short)(cpx > 0 ? cpx : 0); }
                ok = 1;
            } else {
                int pct2 = len_pct(v, vn);
                if (pct2 >= 0) { *dst_pct = (unsigned char)pct2; *dst_px = 0; ok = 1; }
                else { int lok = 0; short px = len_px(v, vn, &lok);
                       if (lok && px > 0) { *dst_px = px; *dst_pct = 0; ok = 1; } }
            }
        } else if (0) {
            int pct = len_pct(v, vn);
            if (pct >= 0) { out->width_pct = (unsigned char)pct; out->width = 0; ok = 1; }
            else { int lok = 0; short px = len_px(v, vn, &lok);
                   if (lok && px > 0) { out->width = px; out->width_pct = 0; ok = 1; } }
        } else if (tok_eq(p, pn, "height")) {
            int pct = len_pct(v, vn);
            if (pct >= 0) { out->height_pct = (unsigned char)pct; out->height = 0; ok = 1; }
            else { int lok = 0; short px = len_px(v, vn, &lok);
                   if (lok && px > 0) { out->height = px; out->height_pct = 0; ok = 1; } }
        } else if (tok_eq(p, pn, "float")) {
            if      (tok_eq(v, vn, "left"))  { out->floatp = VF_LEFT;  ok = 1; }
            else if (tok_eq(v, vn, "right")) { out->floatp = VF_RIGHT; ok = 1; }
            else if (tok_eq(v, vn, "none"))  { out->floatp = VF_NONE;  ok = 1; }
        } else if (tok_eq(p, pn, "clear")) {
            if      (tok_eq(v, vn, "left"))  { out->clearp = 1; ok = 1; }
            else if (tok_eq(v, vn, "right")) { out->clearp = 2; ok = 1; }
            else if (tok_eq(v, vn, "both"))  { out->clearp = 3; ok = 1; }
            else if (tok_eq(v, vn, "none"))  { out->clearp = 0; ok = 1; }
        } else if (tok_eq(p, pn, "position")) {
            if      (tok_eq(v, vn, "relative")) { out->position = VP_RELATIVE; ok = 1; }
            else if (tok_eq(v, vn, "absolute")) { out->position = VP_ABSOLUTE; ok = 1; }
            else if (tok_eq(v, vn, "fixed"))    { out->position = VP_FIXED;    ok = 1; }
            else if (tok_eq(v, vn, "static"))   { out->position = VP_STATIC;   ok = 1; }
            /* sticky behaves as relative until it sticks, and relative is the
             * half we can do -- closer than dropping the rule. */
            else if (tok_eq(v, vn, "sticky"))   { out->position = VP_RELATIVE; ok = 1; }
        } else if (tok_eq(p, pn, "top") || tok_eq(p, pn, "right") ||
                   tok_eq(p, pn, "bottom") || tok_eq(p, pn, "left")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) {
                if      (tok_eq(p, pn, "top"))    { out->ins_top = px;    out->ins_set |= 1; }
                else if (tok_eq(p, pn, "right"))  { out->ins_right = px;  out->ins_set |= 2; }
                else if (tok_eq(p, pn, "bottom")) { out->ins_bottom = px; out->ins_set |= 4; }
                else                              { out->ins_left = px;   out->ins_set |= 8; }
                ok = 1;
            }
        } else if (tok_eq(p, pn, "overflow") || tok_eq(p, pn, "overflow-x") ||
                   tok_eq(p, pn, "overflow-y")) {
            /* hidden/auto/scroll all CLIP here; the difference between them is
             * a scrollbar this renderer does not draw on an arbitrary box. The
             * clipping is the part that changes the layout, and leaving it out
             * is how an overflowing box paints across the rest of the page. */
            if (tok_eq(v, vn, "hidden") || tok_eq(v, vn, "auto") ||
                tok_eq(v, vn, "scroll") || tok_eq(v, vn, "clip")) { out->clip = 1; ok = 1; }
            else if (tok_eq(v, vn, "visible")) { out->clip = 0; ok = 1; }
        } else if (tok_eq(p, pn, "box-sizing")) {
            if (tok_eq(v, vn, "border-box"))      { out->border_box = 1; ok = 1; }
            else if (tok_eq(v, vn, "content-box")) { out->border_box = 0; ok = 1; }
        } else if (tok_eq(p, pn, "max-width")) {
            /* the one percentage worth honouring, because it means "do not
             * overflow me" and that is exactly what an oversized picture does */
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok && px > 0 && (!out->width || px < out->width)) { out->width = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin-top")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->margin_top = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin-bottom")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->margin_bottom = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin-left")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->indent = px; ok = 1; }
        } else if (tok_eq(p, pn, "padding-left")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->pad_left = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin") || tok_eq(p, pn, "padding")) {
            int ispad = tok_eq(p, pn, "padding");
            /* the shorthand, in its four spellings. Only the vertical parts
             * and the left indent are things this renderer can express. */
            size_t k = 0; short vals[4]; int nv = 0;
            while (k < vn && nv < 4) {
                while (k < vn && (v[k]==' '||v[k]=='\t')) k++;
                size_t s2 = k;
                while (k < vn && v[k]!=' ' && v[k]!='\t') k++;
                if (k > s2) { int lok = 0; short px = len_px(v + s2, k - s2, &lok); vals[nv++] = lok ? px : 0; }
            }
            if (nv == 1) { out->margin_top = out->margin_bottom = vals[0]; out->indent = vals[0]; ok = 1; }
            else if (nv == 2) { out->margin_top = out->margin_bottom = vals[0]; out->indent = vals[1]; ok = 1; }
            else if (nv >= 3) { out->margin_top = vals[0]; out->margin_bottom = vals[2];
                                out->indent = nv >= 4 ? vals[3] : vals[1]; ok = 1; }
            if (ok && ispad) {
                /* PADDING IS NOT MARGIN, and once a box can have a background
                 * the difference is visible: padding sits inside the painted
                 * area, margin outside it. They shared a field while nothing
                 * was painted, and that stopped being harmless here. */
                out->pad_top = out->margin_top; out->pad_bottom = out->margin_bottom;
                out->pad_left = out->pad_right = out->indent;
                out->margin_top = out->margin_bottom = out->indent = 0;
            }
        } else if (tok_eq(p, pn, "padding-top") || tok_eq(p, pn, "padding-bottom") ||
                   tok_eq(p, pn, "padding-right")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) {
                if (tok_eq(p, pn, "padding-top")) out->pad_top = px;
                else if (tok_eq(p, pn, "padding-bottom")) out->pad_bottom = px;
                else out->pad_right = px;
                ok = 1;
            }
        } else if (tok_eq(p, pn, "background-color") || tok_eq(p, pn, "background")) {
            /* `background` is a shorthand over image, position, repeat and
             * more; the COLOUR is the part this renderer can honour, so the
             * value is scanned for one and the rest ignored rather than the
             * whole declaration being dropped. */
            unsigned c = css_color(v, vn);
            if (!c) {
                size_t k = 0;
                while (k < vn && !c) {
                    while (k < vn && (v[k]==' '||v[k]=='\t')) k++;
                    size_t s2 = k;
                    int depth = 0;
                    while (k < vn && (depth || (v[k]!=' ' && v[k]!='\t'))) {
                        if (v[k]=='(') depth++;
                        else if (v[k]==')') depth--;
                        k++;
                    }
                    if (k > s2) c = css_color(v + s2, k - s2);
                }
            }
            if (c) { out->bg = c; ok = 1; }
            else if (tok_eq(v, vn, "none") || tok_eq(v, vn, "transparent")) { out->bg = 0; ok = 1; }
        } else if (tok_eq(p, pn, "border-color")) {
            unsigned c = css_color(v, vn);
            if (c) { out->border_color = c; if (!out->border_width) out->border_width = 1; ok = 1; }
        } else if (tok_eq(p, pn, "border-width")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->border_width = px; ok = 1; }
        } else if (tok_eq(p, pn, "border-style")) {
            /* We draw one kind of line. `none`/`hidden` must still be honoured,
             * because that is how a stylesheet TURNS OFF a border it set
             * elsewhere -- ignoring it leaves a box outlined that should not be. */
            if (tok_eq(v, vn, "none") || tok_eq(v, vn, "hidden")) { out->border_width = 0; ok = 1; }
            else ok = 1;
        } else if (tok_eq(p, pn, "border")) {
            /* the shorthand: width, style, colour, in any order */
            if (tok_eq(v, vn, "none") || tok_eq(v, vn, "0")) { out->border_width = 0; ok = 1; }
            else {
                size_t k = 0; short w = 1; unsigned c = 0; int off = 0;
                while (k < vn) {
                    while (k < vn && (v[k]==' '||v[k]=='\t')) k++;
                    size_t s2 = k, depth = 0;
                    while (k < vn && (depth || (v[k]!=' ' && v[k]!='\t'))) {
                        if (v[k]=='(') depth++; else if (v[k]==')') depth--;
                        k++;
                    }
                    if (k <= s2) break;
                    int lok = 0; short px = len_px(v + s2, k - s2, &lok);
                    unsigned cc = css_color(v + s2, k - s2);
                    if (tok_eq(v + s2, k - s2, "none") || tok_eq(v + s2, k - s2, "hidden")) off = 1;
                    else if (cc) c = cc;
                    else if (lok) w = px;
                }
                out->border_width = off ? 0 : w;
                if (c) out->border_color = c;
                ok = 1;
            }
        } else if (tok_eq(p, pn, "border-radius")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok && px >= 0) { out->radius = px; ok = 1; }
        } else if (tok_eq(p, pn, "text-align")) {
            if (tok_eq(v, vn, "center")) { out->align = VA_CENTER; ok = 1; }
            else if (tok_eq(v, vn, "right") || tok_eq(v, vn, "end")) { out->align = VA_RIGHT; ok = 1; }
            else if (tok_eq(v, vn, "left") || tok_eq(v, vn, "start") ||
                     tok_eq(v, vn, "justify")) { out->align = VA_LEFT; ok = 1; }
        } else if (tok_eq(p, pn, "line-height")) {
            /* px/em, or a BARE NUMBER, which CSS defines as a multiple of the
             * font size and is how most stylesheets write it. */
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok && px > 0) { out->line_height = px; ok = 1; }
            else {
                int whole = 0, tenth = 0, seen = 0, dot = 0;
                for (size_t k = 0; k < vn; k++) {
                    char c2 = v[k];
                    if (c2 == '.') { dot = 1; continue; }
                    if (c2 < '0' || c2 > '9') { seen = 0; break; }
                    if (dot) { if (!tenth) tenth = c2 - '0'; }
                    else whole = whole * 10 + (c2 - '0');
                    seen = 1;
                }
                /* against a 15px body: the size the renderer actually uses */
                if (seen && whole < 10) { out->line_height = (short)((whole * 150 + tenth * 15) / 10); ok = 1; }
            }
        }
        if (ok) applied++;
    }
    return applied;
}
