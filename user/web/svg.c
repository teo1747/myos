/* user/web/svg.c -- see svg.h. Parse, flatten, fill. */

#include <string.h>
#include <math.h>
#include "svg.h"

/* ---- bounded working set ------------------------------------------------ *
 * All of it comes out of the caller's scratch, because this runs on a page's
 * behalf and a page is a stranger. A drawing that needs more than this is
 * drawn as far as it fits rather than refused: an icon missing its last
 * flourish still reads as the icon. */
#define SVG_MAX_PTS   4096      /* flattened points in one shape   */
#define SVG_MAX_EDGES 4096
#define SVG_MAX_DEPTH 16        /* <g> nesting                      */
#define SVG_CURVE_STEPS 16      /* subdivisions per bezier segment  */

struct pt { float x, y; };

struct edge {
    float x0, y0, x1, y1;       /* y0 < y1 always */
    int   dir;                  /* +1 if the original ran downwards */
};

/* A 2x3 affine: x' = a x + c y + e,  y' = b x + d y + f. */
struct xf { float a, b, c, d, e, f; };

static const struct xf XF_ID = { 1, 0, 0, 1, 0, 0 };

static struct xf xf_mul(struct xf m, struct xf n) {   /* apply n, then m */
    struct xf r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}
static struct pt xf_apply(struct xf m, float x, float y) {
    struct pt p; p.x = m.a * x + m.c * y + m.e; p.y = m.b * x + m.d * y + m.f;
    return p;
}

/* ---- scanning ----------------------------------------------------------- */

static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int tag_is(const char *s, size_t n, const char *name) {
    size_t m = strlen(name);
    if (n != m) return 0;
    for (size_t i = 0; i < m; i++) if (lc(s[i]) != name[i]) return 0;
    return 1;
}

/* The value of `name` inside one element's attribute text. Returns the length
 * and points `*val` at it, or 0. */
static size_t attr(const char *s, size_t n, const char *name, const char **val) {
    size_t m = strlen(name);
    for (size_t i = 0; i + m + 1 < n; i++) {
        /* must start a token: preceded by whitespace, and followed by '=' */
        if (i && !is_ws(s[i-1])) continue;
        size_t k = 0;
        while (k < m && lc(s[i+k]) == name[k]) k++;
        if (k != m) continue;
        size_t j = i + m;
        while (j < n && is_ws(s[j])) j++;
        if (j >= n || s[j] != '=') continue;
        j++;
        while (j < n && is_ws(s[j])) j++;
        char q = 0;
        if (j < n && (s[j] == '"' || s[j] == '\'')) { q = s[j]; j++; }
        size_t st = j;
        while (j < n && (q ? s[j] != q : !is_ws(s[j]) && s[j] != '>')) j++;
        *val = s + st;
        return j - st;
    }
    return 0;
}

/* A number, in SVG's spelling: optional sign, digits, fraction, exponent. */
static const char *num(const char *p, const char *end, float *out) {
    while (p < end && (is_ws(*p) || *p == ',')) p++;
    if (p >= end) return 0;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    double v = 0; int any = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (p < end && *p == '.') {
        p++; double f = 0.1;
        while (p < end && *p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; p++; any = 1; }
    }
    if (!any) return 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        p++;
        int eneg = 0;
        if (p < end && (*p == '+' || *p == '-')) { eneg = (*p == '-'); p++; }
        int ev = 0, edig = 0;
        while (p < end && *p >= '0' && *p <= '9') { ev = ev * 10 + (*p - '0'); p++; edig = 1; }
        if (!edig) p = save;
        else { double s2 = 1; for (int i = 0; i < ev && i < 60; i++) s2 *= 10; v = eneg ? v / s2 : v * s2; }
    }
    *out = (float)(neg ? -v : v);
    return p;
}

/* ---- colour ------------------------------------------------------------- */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* 0xAARRGGBB, or 0 for `none`. `ink` answers currentColor. */
static unsigned parse_color(const char *s, size_t n, unsigned ink, int *painted) {
    *painted = 1;
    while (n && is_ws(*s)) { s++; n--; }
    while (n && is_ws(s[n-1])) n--;
    if (!n) { *painted = 0; return 0; }
    if (n == 4 && !memcmp(s, "none", 4)) { *painted = 0; return 0; }
    if (n == 12 && lc(s[0]) == 'c' && lc(s[1]) == 'u') return ink;   /* currentColor */
    if (*s == '#') {
        unsigned v = 0; size_t d = 0;
        for (size_t i = 1; i < n; i++) { int h = hexv(s[i]); if (h < 0) break; v = (v << 4) | (unsigned)h; d++; }
        if (d == 3) { unsigned r = (v>>8)&0xF, g = (v>>4)&0xF, b = v&0xF;
                      return 0xFF000000u | (r*17u << 16) | (g*17u << 8) | (b*17u); }
        if (d == 6) return 0xFF000000u | v;
        if (d == 8) return v;          /* #rrggbbaa -> keep, alpha last */
        *painted = 0; return 0;
    }
    /* rgb(r,g,b) */
    if (n > 4 && lc(s[0])=='r' && lc(s[1])=='g' && lc(s[2])=='b') {
        const char *p = s + 3, *end = s + n; float c[3] = {0,0,0};
        while (p < end && *p != '(') p++;
        if (p < end) p++;
        for (int i = 0; i < 3 && p; i++) p = num(p, end, &c[i]);
        return 0xFF000000u | ((unsigned)c[0] << 16) | ((unsigned)c[1] << 8) | (unsigned)c[2];
    }
    static const struct { const char *n; unsigned v; } NAMED[] = {
        {"black",0xFF000000},{"white",0xFFFFFFFF},{"red",0xFFFF0000},
        {"green",0xFF008000},{"blue",0xFF0000FF},{"yellow",0xFFFFFF00},
        {"gray",0xFF808080},{"grey",0xFF808080},{"silver",0xFFC0C0C0},
        {"orange",0xFFFFA500},{"purple",0xFF800080},{"navy",0xFF000080},
        {"teal",0xFF008080},{"lime",0xFF00FF00},{"maroon",0xFF800000},
        {"olive",0xFF808000},{"aqua",0xFF00FFFF},{"fuchsia",0xFFFF00FF},
        {"transparent",0}, {0,0}
    };
    for (int i = 0; NAMED[i].n; i++) {
        size_t m = strlen(NAMED[i].n);
        if (n != m) continue;
        size_t k = 0; while (k < m && lc(s[k]) == NAMED[i].n[k]) k++;
        if (k == m) { if (!NAMED[i].v) *painted = 0; return NAMED[i].v; }
    }
    *painted = 0;
    return 0;
}

/* ---- transform ---------------------------------------------------------- */

static struct xf parse_transform(const char *s, size_t n) {
    struct xf m = XF_ID;
    const char *p = s, *end = s + n;
    while (p < end) {
        while (p < end && (is_ws(*p) || *p == ',')) p++;
        const char *name = p;
        while (p < end && ((lc(*p) >= 'a' && lc(*p) <= 'z'))) p++;
        size_t nl = (size_t)(p - name);
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != '(') break;
        p++;
        float v[6] = {0,0,0,0,0,0};
        int got = 0;
        const char *q = p;
        while (got < 6) {
            const char *r = num(q, end, &v[got]);
            if (!r) break;
            q = r; got++;
        }
        while (q < end && *q != ')') q++;
        if (q < end) q++;
        p = q;
        struct xf t = XF_ID;
        if (tag_is(name, nl, "translate")) { t.e = v[0]; t.f = got > 1 ? v[1] : 0; }
        else if (tag_is(name, nl, "scale")) { t.a = v[0]; t.d = got > 1 ? v[1] : v[0]; }
        else if (tag_is(name, nl, "matrix") && got >= 6) {
            t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5];
        } else if (tag_is(name, nl, "rotate")) {
            float r = v[0] * 3.14159265358979f / 180.0f;
            float cs = cosf(r), sn = sinf(r);
            t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
            if (got >= 3) {   /* rotate about a point */
                struct xf to = XF_ID, back = XF_ID;
                to.e = v[1]; to.f = v[2]; back.e = -v[1]; back.f = -v[2];
                t = xf_mul(to, xf_mul(t, back));
            }
        }
        m = xf_mul(m, t);
    }
    return m;
}

/* ---- the shape being built ---------------------------------------------- */

struct build {
    struct edge *edges;
    int          n_edges, max_edges;
    struct pt   *pts;
    int          n_pts, max_pts;
    struct pt    start;          /* current subpath's first point */
    struct pt    cur;
    int          open;
    struct xf    m;
    int          overflow;
};

static void emit_edge(struct build *b, struct pt a, struct pt c) {
    if (a.y == c.y) return;                       /* horizontal: no crossings */
    if (b->n_edges >= b->max_edges) { b->overflow = 1; return; }
    struct edge *e = &b->edges[b->n_edges++];
    if (a.y < c.y) { e->x0 = a.x; e->y0 = a.y; e->x1 = c.x; e->y1 = c.y; e->dir = 1; }
    else           { e->x0 = c.x; e->y0 = c.y; e->x1 = a.x; e->y1 = a.y; e->dir = -1; }
}

/* A break between subpaths in the stroke polyline. Stroking has to know where
 * one run of points ends and the next begins, or a two-subpath icon grows a
 * line joining them. */
#define SVG_BREAK 1e30f
static int is_break(struct pt p) { return p.x >= SVG_BREAK; }

static void moveto(struct build *b, float x, float y) {
    struct pt p = xf_apply(b->m, x, y);
    if (b->open) emit_edge(b, b->cur, b->start);   /* implicit close */
    b->start = b->cur = p;
    b->open = 1;
    /* THE START POINT IS A POINT. Recording only the lineto targets left a
     * straight two-point path with ONE point in it, and a stroke needs two --
     * so `M0 8 L16 8` with a 4px stroke drew nothing at all. */
    if (b->n_pts && b->n_pts < b->max_pts) {
        struct pt brk = { SVG_BREAK, SVG_BREAK };
        b->pts[b->n_pts++] = brk;
    }
    if (b->n_pts < b->max_pts) b->pts[b->n_pts++] = p;
}
static void lineto(struct build *b, float x, float y) {
    struct pt p = xf_apply(b->m, x, y);
    emit_edge(b, b->cur, p);
    b->cur = p;
    if (b->n_pts < b->max_pts) b->pts[b->n_pts++] = p;
}
static void closepath(struct build *b) {
    if (b->open) emit_edge(b, b->cur, b->start);
    /* ...and the closing segment is part of the OUTLINE too, so a stroked
     * polygon is stroked all the way round rather than left with a gap where
     * it started. */
    if (b->open && b->n_pts < b->max_pts) b->pts[b->n_pts++] = b->start;
    b->cur = b->start;
    b->open = 0;
}
static void cubic(struct build *b, float x0, float y0, float x1, float y1,
                  float x2, float y2, float x3, float y3) {
    for (int i = 1; i <= SVG_CURVE_STEPS; i++) {
        float t = (float)i / SVG_CURVE_STEPS, u = 1 - t;
        float x = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        float y = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        lineto(b, x, y);
    }
}
static void quad(struct build *b, float x0, float y0, float x1, float y1,
                 float x2, float y2) {
    for (int i = 1; i <= SVG_CURVE_STEPS; i++) {
        float t = (float)i / SVG_CURVE_STEPS, u = 1 - t;
        float x = u*u*x0 + 2*u*t*x1 + t*t*x2;
        float y = u*u*y0 + 2*u*t*y1 + t*t*y2;
        lineto(b, x, y);
    }
}

/* Endpoint-parametrised arc -> a run of line segments. The maths is the
 * appendix of the SVG spec; icons use it for rounded corners and pie slices,
 * and dropping it (a straight line to the endpoint) cuts visible chunks out of
 * them. */
static void arcto(struct build *b, float x0, float y0, float rx, float ry,
                  float rot, int large, int sweep, float x, float y) {
    if (rx == 0 || ry == 0) { lineto(b, x, y); return; }
    rx = fabsf(rx); ry = fabsf(ry);
    float phi = rot * 3.14159265358979f / 180.0f;
    float cp = cosf(phi), sp = sinf(phi);
    float dx2 = (x0 - x) / 2, dy2 = (y0 - y) / 2;
    float x1p =  cp * dx2 + sp * dy2;
    float y1p = -sp * dx2 + cp * dy2;
    float rx2 = rx*rx, ry2 = ry*ry, x1p2 = x1p*x1p, y1p2 = y1p*y1p;
    float lam = x1p2/rx2 + y1p2/ry2;
    if (lam > 1) { float s2 = sqrtf(lam); rx *= s2; ry *= s2; rx2 = rx*rx; ry2 = ry*ry; }
    float den = rx2*y1p2 + ry2*x1p2;
    float fac = den > 0 ? (rx2*ry2 - den) / den : 0;
    if (fac < 0) fac = 0;
    float co = sqrtf(fac) * (large == sweep ? -1.0f : 1.0f);
    float cxp =  co * rx * y1p / ry;
    float cyp = -co * ry * x1p / rx;
    float cx = cp*cxp - sp*cyp + (x0 + x)/2;
    float cy = sp*cxp + cp*cyp + (y0 + y)/2;
    float t1 = atan2f((y1p - cyp)/ry, (x1p - cxp)/rx);
    float t2 = atan2f((-y1p - cyp)/ry, (-x1p - cxp)/rx);
    float dt = t2 - t1;
    const float TAU = 6.28318530717959f;
    if (!sweep && dt > 0) dt -= TAU;
    else if (sweep && dt < 0) dt += TAU;
    int steps = (int)(fabsf(dt) / (TAU / 64)) + 2;
    if (steps > 128) steps = 128;
    for (int i = 1; i <= steps; i++) {
        float t = t1 + dt * (float)i / (float)steps;
        float px = cx + rx * cosf(t) * cp - ry * sinf(t) * sp;
        float py = cy + rx * cosf(t) * sp + ry * sinf(t) * cp;
        lineto(b, px, py);
    }
}

/* ---- the path grammar --------------------------------------------------- */

static void parse_path(struct build *b, const char *d, size_t n) {
    const char *p = d, *end = d + n;
    float cx = 0, cy = 0;          /* current point, USER space */
    float sx = 0, sy = 0;          /* subpath start, user space */
    float px = 0, py = 0;          /* previous control, for S/T */
    char prev = 0;
    while (p < end) {
        while (p < end && (is_ws(*p) || *p == ',')) p++;
        if (p >= end) break;
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { p++; }
        else {
            /* a repeated command: M implies L, m implies l, others repeat */
            c = prev == 'M' ? 'L' : prev == 'm' ? 'l' : prev;
            if (!c) break;
        }
        int rel = (c >= 'a' && c <= 'z');
        char C = (char)(rel ? c - 'a' + 'A' : c);
        float v[7];
        switch (C) {
        case 'M': {
            if (!(p = num(p, end, &v[0]))) return;
            if (!(p = num(p, end, &v[1]))) return;
            cx = rel ? cx + v[0] : v[0]; cy = rel ? cy + v[1] : v[1];
            sx = cx; sy = cy;
            moveto(b, cx, cy);
            px = cx; py = cy;
            break; }
        case 'L': {
            if (!(p = num(p, end, &v[0]))) return;
            if (!(p = num(p, end, &v[1]))) return;
            cx = rel ? cx + v[0] : v[0]; cy = rel ? cy + v[1] : v[1];
            lineto(b, cx, cy); px = cx; py = cy;
            break; }
        case 'H': {
            if (!(p = num(p, end, &v[0]))) return;
            cx = rel ? cx + v[0] : v[0];
            lineto(b, cx, cy); px = cx; py = cy;
            break; }
        case 'V': {
            if (!(p = num(p, end, &v[0]))) return;
            cy = rel ? cy + v[0] : v[0];
            lineto(b, cx, cy); px = cx; py = cy;
            break; }
        case 'C': {
            for (int i = 0; i < 6; i++) if (!(p = num(p, end, &v[i]))) return;
            float x1 = rel ? cx+v[0] : v[0], y1 = rel ? cy+v[1] : v[1];
            float x2 = rel ? cx+v[2] : v[2], y2 = rel ? cy+v[3] : v[3];
            float x3 = rel ? cx+v[4] : v[4], y3 = rel ? cy+v[5] : v[5];
            cubic(b, cx, cy, x1, y1, x2, y2, x3, y3);
            px = x2; py = y2; cx = x3; cy = y3;
            break; }
        case 'S': {
            for (int i = 0; i < 4; i++) if (!(p = num(p, end, &v[i]))) return;
            float x1 = 2*cx - px, y1 = 2*cy - py;        /* reflected control */
            float x2 = rel ? cx+v[0] : v[0], y2 = rel ? cy+v[1] : v[1];
            float x3 = rel ? cx+v[2] : v[2], y3 = rel ? cy+v[3] : v[3];
            cubic(b, cx, cy, x1, y1, x2, y2, x3, y3);
            px = x2; py = y2; cx = x3; cy = y3;
            break; }
        case 'Q': {
            for (int i = 0; i < 4; i++) if (!(p = num(p, end, &v[i]))) return;
            float x1 = rel ? cx+v[0] : v[0], y1 = rel ? cy+v[1] : v[1];
            float x2 = rel ? cx+v[2] : v[2], y2 = rel ? cy+v[3] : v[3];
            quad(b, cx, cy, x1, y1, x2, y2);
            px = x1; py = y1; cx = x2; cy = y2;
            break; }
        case 'T': {
            for (int i = 0; i < 2; i++) if (!(p = num(p, end, &v[i]))) return;
            float x1 = 2*cx - px, y1 = 2*cy - py;
            float x2 = rel ? cx+v[0] : v[0], y2 = rel ? cy+v[1] : v[1];
            quad(b, cx, cy, x1, y1, x2, y2);
            px = x1; py = y1; cx = x2; cy = y2;
            break; }
        case 'A': {
            for (int i = 0; i < 7; i++) if (!(p = num(p, end, &v[i]))) return;
            float ex = rel ? cx+v[5] : v[5], ey = rel ? cy+v[6] : v[6];
            arcto(b, cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, ex, ey);
            cx = ex; cy = ey; px = cx; py = cy;
            break; }
        case 'Z':
            closepath(b);
            cx = sx; cy = sy; px = cx; py = cy;
            break;
        default:
            return;                                   /* unknown: stop here */
        }
        prev = c;
        if (b->overflow) return;
    }
    if (b->open) closepath(b);
}

/* ---- rasterising -------------------------------------------------------- */

#define SUB 4      /* sub-scanlines per pixel row */

struct raster {
    uint32_t *out;
    uint32_t  w, h;
    float    *cov;      /* one row of coverage, w floats */
};

/* Fill the accumulated edges into the target with `color`, by `evenodd`. */
static void fill_edges(struct raster *r, struct build *b, unsigned color, int evenodd) {
    if (b->n_edges <= 0) return;
    unsigned a8 = (color >> 24) & 0xFF;
    if (!a8) return;
    float cr = (float)((color >> 16) & 0xFF);
    float cg = (float)((color >> 8) & 0xFF);
    float cb = (float)(color & 0xFF);
    float ca = (float)a8 / 255.0f;

    /* The drawing's vertical extent, so a small icon in a big box costs only
     * the rows it touches. */
    float ymin = b->edges[0].y0, ymax = b->edges[0].y1;
    for (int i = 1; i < b->n_edges; i++) {
        if (b->edges[i].y0 < ymin) ymin = b->edges[i].y0;
        if (b->edges[i].y1 > ymax) ymax = b->edges[i].y1;
    }
    int y_lo = (int)floorf(ymin); if (y_lo < 0) y_lo = 0;
    int y_hi = (int)ceilf(ymax);  if (y_hi > (int)r->h) y_hi = (int)r->h;

    for (int y = y_lo; y < y_hi; y++) {
        for (uint32_t i = 0; i < r->w; i++) r->cov[i] = 0.0f;
        int any = 0;
        for (int s = 0; s < SUB; s++) {
            float sy = (float)y + ((float)s + 0.5f) / SUB;
            /* crossings on this sub-scanline, with winding direction */
            float xs[128]; int dirs[128]; int nx = 0;
            for (int i = 0; i < b->n_edges && nx < 128; i++) {
                struct edge *e = &b->edges[i];
                if (sy < e->y0 || sy >= e->y1) continue;
                float t = (sy - e->y0) / (e->y1 - e->y0);
                xs[nx] = e->x0 + t * (e->x1 - e->x0);
                dirs[nx] = e->dir;
                nx++;
            }
            if (nx < 2) continue;
            /* insertion sort: nx is small and nearly sorted between rows */
            for (int i = 1; i < nx; i++) {
                float kx = xs[i]; int kd = dirs[i]; int j = i - 1;
                while (j >= 0 && xs[j] > kx) { xs[j+1] = xs[j]; dirs[j+1] = dirs[j]; j--; }
                xs[j+1] = kx; dirs[j+1] = kd;
            }
            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += evenodd ? 1 : dirs[i];
                int inside = evenodd ? (((i + 1) & 1) != 0) : (wind != 0);
                if (!inside) continue;
                float xa = xs[i], xb = xs[i+1];
                if (xb <= 0 || xa >= (float)r->w) continue;
                if (xa < 0) xa = 0;
                if (xb > (float)r->w) xb = (float)r->w;
                if (xb <= xa) continue;
                any = 1;
                /* horizontal coverage, with partial pixels at both ends */
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) { r->cov[ia] += (xb - xa) / SUB; continue; }
                r->cov[ia] += ((float)(ia + 1) - xa) / SUB;
                for (int px2 = ia + 1; px2 < ib; px2++) r->cov[px2] += 1.0f / SUB;
                if (ib < (int)r->w) r->cov[ib] += (xb - (float)ib) / SUB;
            }
        }
        if (!any) continue;
        uint32_t *row = r->out + (size_t)y * r->w;
        for (uint32_t x = 0; x < r->w; x++) {
            float c = r->cov[x];
            if (c <= 0.0f) continue;
            if (c > 1.0f) c = 1.0f;
            float sa = ca * c;                         /* source alpha */
            /* premultiplied over */
            uint32_t dpx = row[x];
            float da = (float)((dpx >> 24) & 0xFF) / 255.0f;
            float dr = (float)((dpx >> 16) & 0xFF);
            float dg = (float)((dpx >> 8) & 0xFF);
            float db = (float)(dpx & 0xFF);
            float inv = 1.0f - sa;
            float or_ = cr * sa + dr * inv;
            float og  = cg * sa + dg * inv;
            float ob  = cb * sa + db * inv;
            float oa  = sa + da * inv;
            uint32_t R = (uint32_t)(or_ + 0.5f), G = (uint32_t)(og + 0.5f);
            uint32_t B = (uint32_t)(ob + 0.5f), A = (uint32_t)(oa * 255.0f + 0.5f);
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            if (A > 255) A = 255;
            row[x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
}

/* ---- stroking ----------------------------------------------------------- *
 * A stroke as a quad per segment. Not joins-and-caps correct -- an icon drawn
 * with a 2px stroke does not show the difference, and the alternative is an
 * outline offsetter, which is most of a vector library. */
static void stroke_points(struct build *b, const struct pt *pts, int n, float w) {
    if (n < 2 || w <= 0) return;
    float hw = w / 2;
    for (int i = 0; i + 1 < n; i++) {
        if (is_break(pts[i]) || is_break(pts[i+1])) continue;   /* subpath gap */
        float dx = pts[i+1].x - pts[i].x, dy = pts[i+1].y - pts[i].y;
        float l = sqrtf(dx*dx + dy*dy);
        if (l < 0.0001f) continue;
        float nx = -dy / l * hw, ny = dx / l * hw;
        struct pt a = { pts[i].x + nx,   pts[i].y + ny };
        struct pt c = { pts[i+1].x + nx, pts[i+1].y + ny };
        struct pt e = { pts[i+1].x - nx, pts[i+1].y - ny };
        struct pt f = { pts[i].x - nx,   pts[i].y - ny };
        emit_edge(b, a, c); emit_edge(b, c, e);
        emit_edge(b, e, f); emit_edge(b, f, a);
    }
}

/* ---- the document ------------------------------------------------------- */

struct gstate { struct xf m; unsigned fill; int fill_set; float opacity; };

static const char *find_svg(const uint8_t *src, size_t len) {
    const char *s = (const char *)src;
    for (size_t i = 0; i + 4 < len; i++)
        if (s[i] == '<' && lc(s[i+1]) == 's' && lc(s[i+2]) == 'v' && lc(s[i+3]) == 'g' &&
            (is_ws(s[i+4]) || s[i+4] == '>')) return s + i;
    return 0;
}

int svg_probe(const uint8_t *src, size_t len, uint32_t *w, uint32_t *h) {
    const char *tag = find_svg(src, len);
    if (!tag) return -1;
    const char *end = (const char *)src + len;
    const char *gt = tag;
    while (gt < end && *gt != '>') gt++;
    size_t n = (size_t)(gt - tag);
    float fw = 0, fh = 0;
    const char *v;
    size_t vn = attr(tag, n, "width", &v);
    if (vn) num(v, v + vn, &fw);
    vn = attr(tag, n, "height", &v);
    if (vn) num(v, v + vn, &fh);
    if (fw <= 0 || fh <= 0) {
        vn = attr(tag, n, "viewbox", &v);
        if (vn) {
            float b[4] = {0,0,0,0};
            const char *p = v, *e2 = v + vn;
            for (int i = 0; i < 4 && p; i++) p = num(p, e2, &b[i]);
            if (b[2] > 0 && b[3] > 0) { fw = b[2]; fh = b[3]; }
        }
    }
    /* A document with no size at all still has to be given one: 24px is what
     * an icon is, and the caller scales it to the box anyway. */
    if (fw <= 0 || fh <= 0) { fw = 24; fh = 24; }
    if (fw > 4096) fw = 4096;
    if (fh > 4096) fh = 4096;
    if (w) *w = (uint32_t)fw;
    if (h) *h = (uint32_t)fh;
    return 0;
}

int svg_render(const uint8_t *src, size_t len, uint32_t *out, size_t out_bytes,
               uint8_t *scratch, size_t scratch_bytes,
               uint32_t w, uint32_t h, uint32_t ink) {
    if (!out || !src || w == 0 || h == 0) return -1;
    if (out_bytes < (size_t)w * h * 4) return -1;
    size_t need = sizeof(struct edge) * SVG_MAX_EDGES
                + sizeof(struct pt) * SVG_MAX_PTS
                + sizeof(float) * w;
    if (!scratch || scratch_bytes < need) return -1;

    const char *tag = find_svg(src, len);
    if (!tag) return -1;
    const char *end = (const char *)src + len;

    memset(out, 0, (size_t)w * h * 4);

    struct edge *edges = (struct edge *)scratch;
    struct pt   *pts   = (struct pt *)(edges + SVG_MAX_EDGES);
    float       *cov   = (float *)(pts + SVG_MAX_PTS);

    struct raster R = { out, w, h, cov };

    /* THE VIEWBOX, which is what makes an icon fit its box: the drawing is in
     * the document's own units and has to be mapped onto the pixels we were
     * asked for. Without it a 24-unit icon rendered into a 24px box only by
     * coincidence, and every icon drawn at any other size fell off the edge. */
    const char *gt = tag;
    while (gt < end && *gt != '>') gt++;
    size_t rootn = (size_t)(gt - tag);
    float vb[4] = { 0, 0, 0, 0 };
    const char *v;
    size_t vn = attr(tag, rootn, "viewbox", &v);
    if (vn) {
        const char *p = v, *e2 = v + vn;
        for (int i = 0; i < 4 && p; i++) p = num(p, e2, &vb[i]);
    }
    if (vb[2] <= 0 || vb[3] <= 0) { vb[0] = vb[1] = 0; vb[2] = (float)w; vb[3] = (float)h; }
    struct xf root = XF_ID;
    root.a = (float)w / vb[2];
    root.d = (float)h / vb[3];
    root.e = -vb[0] * root.a;
    root.f = -vb[1] * root.d;

    struct gstate stack[SVG_MAX_DEPTH];
    int sp = 0;
    stack[0].m = root;
    stack[0].fill = ink; stack[0].fill_set = 0; stack[0].opacity = 1.0f;

    const char *p = gt < end ? gt + 1 : end;
    while (p < end) {
        while (p < end && *p != '<') p++;
        if (p >= end) break;
        p++;
        if (p < end && *p == '/') {              /* a closing tag */
            p++;
            const char *nm = p;
            while (p < end && *p != '>' && !is_ws(*p)) p++;
            if (tag_is(nm, (size_t)(p - nm), "g") && sp > 0) sp--;
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }
        if (p < end && (*p == '!' || *p == '?')) {   /* comment or PI */
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }
        const char *nm = p;
        while (p < end && *p != '>' && !is_ws(*p) && *p != '/') p++;
        size_t nl = (size_t)(p - nm);
        const char *abeg = p;
        while (p < end && *p != '>') p++;
        size_t an = (size_t)(p - abeg);
        int self_closed = (p > abeg && p[-1] == '/');
        if (p < end) p++;

        struct gstate gs = stack[sp];
        /* transform= and fill= are inherited, so they are read for every
         * element and pushed only by <g>. */
        size_t tn = attr(abeg, an, "transform", &v);
        if (tn) gs.m = xf_mul(gs.m, parse_transform(v, tn));
        size_t fn = attr(abeg, an, "fill", &v);
        if (fn) { int painted; unsigned c = parse_color(v, fn, ink, &painted);
                  gs.fill = c; gs.fill_set = painted ? 1 : 2; }   /* 2 = none */
        size_t on = attr(abeg, an, "opacity", &v);
        if (on) { float o = 1; num(v, v + on, &o); gs.opacity *= o; }

        if (tag_is(nm, nl, "g")) {
            if (!self_closed && sp + 1 < SVG_MAX_DEPTH) stack[++sp] = gs;
            continue;
        }
        if (tag_is(nm, nl, "defs") || tag_is(nm, nl, "clippath") ||
            tag_is(nm, nl, "mask")  || tag_is(nm, nl, "lineargradient") ||
            tag_is(nm, nl, "radialgradient") || tag_is(nm, nl, "style") ||
            tag_is(nm, nl, "text")  || tag_is(nm, nl, "title") ||
            tag_is(nm, nl, "desc")) {
            /* Not drawn, and their CONTENTS must not be drawn either -- a
             * <defs> full of paths would otherwise be painted over the icon. */
            if (!self_closed) {
                char close[24];
                size_t cl = nl < 20 ? nl : 20;
                close[0] = '<'; close[1] = '/';
                for (size_t i = 0; i < cl; i++) close[2+i] = lc(nm[i]);
                close[2+cl] = 0;
                size_t clen = cl + 2;
                while (p + clen <= end) {
                    size_t k = 0;
                    while (k < clen && lc(p[k]) == close[k]) k++;
                    if (k == clen) { p += clen; while (p < end && *p != '>') p++; if (p<end) p++; break; }
                    p++;
                }
            }
            continue;
        }

        /* ---- a drawable shape ---- */
        struct build b = { edges, 0, SVG_MAX_EDGES, pts, 0, SVG_MAX_PTS,
                           {0,0}, {0,0}, 0, gs.m, 0 };
        int is_shape = 1;
        if (tag_is(nm, nl, "path")) {
            size_t dn = attr(abeg, an, "d", &v);
            if (dn) parse_path(&b, v, dn);
        } else if (tag_is(nm, nl, "rect")) {
            float x = 0, y = 0, rw = 0, rh = 0;
            size_t k;
            if ((k = attr(abeg, an, "x", &v))) num(v, v+k, &x);
            if ((k = attr(abeg, an, "y", &v))) num(v, v+k, &y);
            if ((k = attr(abeg, an, "width", &v))) num(v, v+k, &rw);
            if ((k = attr(abeg, an, "height", &v))) num(v, v+k, &rh);
            if (rw > 0 && rh > 0) {
                moveto(&b, x, y); lineto(&b, x+rw, y);
                lineto(&b, x+rw, y+rh); lineto(&b, x, y+rh); closepath(&b);
            }
        } else if (tag_is(nm, nl, "circle") || tag_is(nm, nl, "ellipse")) {
            float cxv = 0, cyv = 0, rx = 0, ry = 0;
            size_t k;
            if ((k = attr(abeg, an, "cx", &v))) num(v, v+k, &cxv);
            if ((k = attr(abeg, an, "cy", &v))) num(v, v+k, &cyv);
            if ((k = attr(abeg, an, "r", &v)))  { num(v, v+k, &rx); ry = rx; }
            if ((k = attr(abeg, an, "rx", &v))) num(v, v+k, &rx);
            if ((k = attr(abeg, an, "ry", &v))) num(v, v+k, &ry);
            if (rx > 0 && ry > 0) {
                moveto(&b, cxv + rx, cyv);
                for (int i = 1; i <= 64; i++) {
                    float t = 6.28318530717959f * (float)i / 64.0f;
                    lineto(&b, cxv + rx * cosf(t), cyv + ry * sinf(t));
                }
                closepath(&b);
            }
        } else if (tag_is(nm, nl, "line")) {
            float x1=0,y1=0,x2=0,y2=0; size_t k;
            if ((k = attr(abeg, an, "x1", &v))) num(v, v+k, &x1);
            if ((k = attr(abeg, an, "y1", &v))) num(v, v+k, &y1);
            if ((k = attr(abeg, an, "x2", &v))) num(v, v+k, &x2);
            if ((k = attr(abeg, an, "y2", &v))) num(v, v+k, &y2);
            moveto(&b, x1, y1); lineto(&b, x2, y2);
            b.open = 0;                              /* a line does not close */
        } else if (tag_is(nm, nl, "polygon") || tag_is(nm, nl, "polyline")) {
            size_t k = attr(abeg, an, "points", &v);
            if (k) {
                const char *q = v, *e2 = v + k;
                float x, y; int first = 1;
                while (q) {
                    const char *r2 = num(q, e2, &x); if (!r2) break;
                    r2 = num(r2, e2, &y); if (!r2) break;
                    if (first) { moveto(&b, x, y); first = 0; } else lineto(&b, x, y);
                    q = r2;
                }
                if (tag_is(nm, nl, "polygon")) closepath(&b);
                else b.open = 0;
            }
        } else {
            is_shape = 0;
        }
        if (!is_shape) continue;

        /* FILL, then STROKE, which is the order SVG paints them. */
        unsigned fill = gs.fill;
        int fill_none = (gs.fill_set == 2);
        if (!gs.fill_set) fill = ink;               /* the initial value is black/ink */
        if (gs.opacity < 1.0f) {
            unsigned a2 = (unsigned)(((fill >> 24) & 0xFF) * gs.opacity);
            fill = (fill & 0x00FFFFFFu) | (a2 << 24);
        }
        int evenodd = 0;
        size_t rn = attr(abeg, an, "fill-rule", &v);
        if (rn && rn >= 7 && lc(v[0]) == 'e') evenodd = 1;

        int n_fill_edges = b.n_edges;
        if (!fill_none && n_fill_edges > 0) fill_edges(&R, &b, fill, evenodd);

        size_t sn = attr(abeg, an, "stroke", &v);
        if (sn) {
            int painted; unsigned sc = parse_color(v, sn, ink, &painted);
            if (painted) {
                float sw = 1.0f;
                size_t wn = attr(abeg, an, "stroke-width", &v);
                if (wn) num(v, v + wn, &sw);
                /* the stroke is in USER units; scale by the transform */
                float scale = sqrtf(fabsf(gs.m.a * gs.m.d - gs.m.b * gs.m.c));
                if (scale <= 0) scale = 1;
                struct build sb = { edges, 0, SVG_MAX_EDGES, pts, b.n_pts, SVG_MAX_PTS,
                                    {0,0}, {0,0}, 0, gs.m, 0 };
                stroke_points(&sb, pts, b.n_pts, sw * scale);
                if (gs.opacity < 1.0f) {
                    unsigned a2 = (unsigned)(((sc >> 24) & 0xFF) * gs.opacity);
                    sc = (sc & 0x00FFFFFFu) | (a2 << 24);
                }
                /* nonzero would cancel overlapping quads; even-odd would hole
                 * them. Each quad is filled on its own winding, so nonzero is
                 * right as long as they are all wound the same way -- they are,
                 * because stroke_points emits them in a fixed order. */
                fill_edges(&R, &sb, sc, 0);
            }
        }
    }
    return 0;
}

int svg_decode(const uint8_t *src, size_t len, uint32_t *out, size_t out_bytes,
               uint8_t *scratch, size_t scratch_bytes, uint32_t *w, uint32_t *h) {
    uint32_t pw = 0, ph = 0;
    if (svg_probe(src, len, &pw, &ph) != 0) return -1;
    if (w) *w = pw;
    if (h) *h = ph;
    /* Black is the SVG initial fill, and an icon with no fill stated is meant
     * to be drawn in the surrounding text colour -- which an <img> has no way
     * to know, so black it is. */
    return svg_render(src, len, out, out_bytes, scratch, scratch_bytes,
                      pw, ph, 0xFF000000u);
}
