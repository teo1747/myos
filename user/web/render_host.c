/* user/web/render_host.c -- run the browser's render path on the HOST.
 *
 * Everything from the document bytes to resolved pixel geometry is plain
 * userland C with no syscalls in it: html.c parses, style.c computes, render.c
 * emits EmUI nodes, and layout resolves them. Only the window and the network
 * need the OS. So the entire pipeline can be driven from a host `main`, which
 * is what this file is -- and a bug that takes a five-minute boot to look at
 * takes two seconds to look at here.
 *
 * It prints the resolved tree (kind, absolute rect, text) and optionally
 * renders a PNG, so "is the layout wrong or is the emission wrong?" is a
 * question you answer by reading, not by squinting at a screenshot.
 *
 *   make browser-render                     -- the start page
 *   make browser-render DOC=path W=940      -- any document, any width
 */
#include "em.h"
#include "scene_render.h"
#include "font.h"
#include "html.h"
#include "style.h"
#include "render.h"
#include "css.h"
#include "imgcache.h"
#include "png.h"
#include "jpeg.h"
#include "select.h"
#include "cssref.h"
#include "find.h"
#include "net.h"
#include "fetchjob.h"
#include "jsdom.h"
#include "url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 0; }
    fclose(f); buf[n] = 0; *len = (size_t)n; return buf;
}

/* ---- the document ------------------------------------------------------ */

#define NODE_MAX 16384
#define STR_MAX  (1024 * 1024)
static struct html_node g_nodes[NODE_MAX];
static char             g_strs[STR_MAX];
static struct html_doc  g_doc;
static int              g_root = -1;
static float            g_scroll = 0;
static struct css_sheet g_sheet;
static int HDEFER;   /* host cache: pretend no picture has arrived yet */
static const char      *g_doc_base;

/* The app's shape, verbatim from user/bin/vellum.c -- if this diverges the
 * harness stops being evidence. `g_busy` stands in for a fetch in flight, so
 * the loading strip can be laid out here instead of in a five-minute boot. */
static int  g_busy;
static char g_bar[512] = "https://valid-isrgrootx1.letsencrypt.org/";

/* The app registers one, and until this harness did too, every link rendered
 * here as plain Text while on the metal it renders as a BUTTON -- so the whole
 * button path was untested on the fast loop and only ever exercised in a
 * five-minute boot. */
static void host_on_link(const char *href) { (void)href; }

static void app(void) {
    Window("Vellum") {
        AppBar("Vellum") {
            IconButton(IconChevronL);
            IconButton(IconChevronR);
            IconButton(IconArrowR);
        }
        HStack(.spacing = 8, .align = Center, .px = 12, .py = 6) {
            TextField(g_bar, sizeof g_bar, "Path or URL");
            Button("Open").primary().font(Caption).py(2);
        }
        Divider();

        if (g_busy) {
            HStack(.spacing = 8, .align = Center, .px = 12, .py = 4) {
                Spinner();
                Text("Loading https://valid-isrgrootx1.letsencrypt.org/   1.2s")
                    .caption().secondary();
            }
            Divider();
        }

        ScrollView(&g_scroll, em_viewport_height() - (g_busy ? 164.0f : 132.0f)) {
            VStack(.spacing = 0, .align = Fill, .padding = 22, .grow = 1) {
                vellum_render_sized(&g_doc, g_root, &g_sheet, g_doc_base,
                                    em_viewport_width() - 44.0f);
            }
        }

        Divider();
        HStack(.spacing = 10, .align = Center, .px = 12, .py = 4) {
            Text("200  https (authenticated)  4067 bytes").caption().tertiary();
            Spacer();
            Text(g_bar).caption().tertiary();
        }
    }
}

/* ---- the dump ---------------------------------------------------------- */

static struct scene_arena  sa;
static struct layout_arena la;

/* The lowest text INSIDE THE SCROLL AREA, and how many text nodes are there.
 *
 * Inside, specifically: the first attempt walked the whole window and kept
 * finding y=688.6 on every page, because the lowest text in a browser window
 * is the status bar -- furniture that never moves no matter what the document
 * does. A reflow check that measures the chrome cannot fail, which is the
 * worst property a check can have. The document lives under the first
 * clipping node (the ScrollView), so the walk only counts once it is inside
 * one. Layout positions everything whether or not it is clipped away, so text
 * below the fold still reports an honest y. */
static void text_extent(struct node_handle h, float oy, int inside,
                        float *maxy, int *count) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float y = oy + n->ty;
    if (n->clip_children) inside = 1;
    if (inside && n->kind == SCENE_NODE_TEXT) {
        if (y > *maxy) *maxy = y;
        (*count)++;
    }
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        text_extent(c, y, inside, maxy, count);
        c = cn->next_sibling;
    }
}

/* The first and last TEXT nodes inside the document's clip, in tree order --
 * which is document order. Used to check a copy came out in the right order
 * without naming any particular page's words. */
static void first_last_text(struct node_handle h, int inside,
                            struct node_handle *first, struct node_handle *last) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    if (n->clip_children) inside = 1;
    if (inside && n->kind == SCENE_NODE_TEXT && n->data.text.utf8 && n->data.text.utf8[0]) {
        if (node_handle_is_null(*first)) *first = h;
        *last = h;
    }
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        first_last_text(c, inside, first, last);
        c = cn->next_sibling;
    }
}

/* The DOM, before any styling or layout touches it. The header of this file
 * promises you can tell whether a fault is in the parse or in the emission --
 * and until this existed you could only see the emission, so every parse bug
 * had to be inferred from the shape of the wreckage downstream. Printed when
 * DOM=1 is set. */
/* Text bytes under `node`, so a hidden subtree can be reported by how much of
 * the page it takes with it. */
static int subtree_text(struct html_doc *d, int node) {
    if (node < 0 || node >= d->n) return 0;
    int n = 0;
    if (d->nodes[node].kind == HTML_TEXT && d->nodes[node].text)
        n = (int)strlen(d->nodes[node].text);
    for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling)
        n += subtree_text(d, c);
    return n;
}

static float g_tall_min = 2000;

/* node -> the instance box render.c gave it, filled by the box hook. */
static struct { unsigned idx, gen; } g_box[NODE_MAX];
static float g_box_y[NODE_MAX];
static void box_hook(int node, unsigned idx, unsigned gen) {
    if (node >= 0 && node < NODE_MAX) { g_box[node].idx = idx; g_box[node].gen = gen; }
}

static void dump_tall(struct html_doc *d, struct scene_arena *sa) {
    struct tall_ent { float h; int node; };
    struct tall_ent top[12];
    int n = 0;
    for (int i = 0; i < d->n; i++) {
        if (d->nodes[i].kind != HTML_ELEM || !g_box[i].idx) continue;
        struct instance_handle h = { g_box[i].idx, g_box[i].gen };
        struct scene_node *sn = scene_resolve(sa, ui_scene_of(h));
        float hgt = sn ? sn->height : -1;
        g_box_y[i] = sn ? sn->ty : 0;
        if (hgt < g_tall_min) continue;
        int at = n < 12 ? n++ : 11;
        top[at].h = hgt; top[at].node = i;
        for (int k = at; k > 0 && top[k - 1].h < top[k].h; k--) {
            struct tall_ent t = top[k - 1]; top[k - 1] = top[k]; top[k] = t;
        }
    }
    for (int i = 0; i < n; i++) {
        int k = top[i].node;
        printf("TALL| %9.0f px  box=%-5u y=%-9.0f <%s%s%s%s%s>\n", top[i].h,
               g_box[k].idx, g_box_y[k], d->nodes[k].tag,
               d->nodes[k].id ? " id=" : "", d->nodes[k].id ? d->nodes[k].id : "",
               d->nodes[k].klass ? " class=" : "",
               d->nodes[k].klass ? d->nodes[k].klass : "");
    }
}

static void dump_grids(struct html_doc *d, int node, const struct vstyle *parent,
                       int depth) {
    if (node < 0 || node >= d->n || depth > 40) return;
    struct vstyle s = *parent;
    if (d->nodes[node].kind == HTML_ELEM) {
        vstyle_for_node(d, node, parent, &g_sheet, &s);
        if (s.display == VD_GRID) {
            printf("GRID| <%s%s%s> cols=%d tracks=%d areas=%d\n",
                   d->nodes[node].tag,
                   d->nodes[node].klass ? " class=" : "",
                   d->nodes[node].klass ? d->nodes[node].klass : "",
                   s.grid_cols, s.grid_ntrack, s.n_areas);
            for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling) {
                if (d->nodes[c].kind != HTML_ELEM) continue;
                struct vstyle cs;
                vstyle_for_node(d, c, &s, &g_sheet, &cs);
                int placed = 0;
                for (int a = 0; a < s.n_areas; a++)
                    if (cs.grid_area && s.area_name[a] == cs.grid_area) { placed = 1; break; }
                printf("GRID|   child <%s%s%s> area=%u %s\n", d->nodes[c].tag,
                       d->nodes[c].klass ? " class=" : "",
                       d->nodes[c].klass ? d->nodes[c].klass : "",
                       cs.grid_area, placed ? "PLACED" : "(auto-flow)");
            }
        }
    }
    for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling)
        dump_grids(d, c, &s, depth + 1);
}

static void dump_hidden(struct html_doc *d, int node, const struct vstyle *parent,
                        int depth) {
    if (node < 0 || node >= d->n || depth > 40) return;
    struct vstyle s = *parent;
    if (d->nodes[node].kind == HTML_ELEM) {
        vstyle_for_node(d, node, parent, &g_sheet, &s);
        if (s.display == VD_NONE) {
            int chars = subtree_text(d, node);
            /* <head>, <script> and friends are display:none by the UA sheet and
             * carry no text worth reporting -- only say something when a
             * VISIBLE part of the page disappeared. */
            if (chars > 0) {
                printf("HIDDEN| depth=%-2d <%s%s%s%s%s> hides %d chars of text\n",
                       depth, d->nodes[node].tag,
                       d->nodes[node].id ? " id=" : "", d->nodes[node].id ? d->nodes[node].id : "",
                       d->nodes[node].klass ? " class=" : "", d->nodes[node].klass ? d->nodes[node].klass : "",
                       chars);
                /* ...and WHICH RULE did it. Knowing that the body is hidden
                 * still leaves a stylesheet to search; knowing the selector
                 * that matched ends the search. */
                for (int r = 0; r < g_sheet.n; r++) {
                    const struct css_rule *ru = &g_sheet.rules[r];
                    if (!css_sel_match(&ru->sel, d, node)) continue;
                    /* every matching rule, not only the ones that mention display:
                     * the point is to see what the cascade actually applied. */
                    printf("HIDDEN|   matched by spec=%u: ", ru->sel.spec);
                    for (int k = 0; k < ru->sel.n; k++)
                        printf("%s%s%s%s%s ",
                               k ? (ru->sel.part[k].comb == CSS_COMB_CHILD ? "> " :
                                    ru->sel.part[k].comb == CSS_COMB_ADJ   ? "+ " :
                                    ru->sel.part[k].comb == CSS_COMB_SIB   ? "~ " : "") : "",
                               ru->sel.part[k].tag,
                               ru->sel.part[k].id[0] ? "#" : "", ru->sel.part[k].id,
                               ru->sel.part[k].klass[0] ? "." : "");
                    printf("  {%.*s}\n", (int)(ru->decls_len > 90 ? 90 : ru->decls_len), ru->decls);
                }
            }
            return;                     /* its children are hidden with it */
        }
    }
    for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling)
        dump_hidden(d, c, &s, depth + 1);
}

static void dump_dom(struct html_doc *d, int node, int depth) {
    if (node < 0 || node >= d->n || depth > 40) return;
    struct html_node *n = &d->nodes[node];
    if (n->kind == HTML_TEXT) {
        const char *t = n->text ? n->text : "";
        printf("%*s#text \"%.60s\"\n", depth * 2, "", t);
    } else {
        printf("%*s<%s>", depth * 2, "", n->tag);
        if (n->klass) printf(" class=%s", n->klass);
        if (n->href)  printf(" href=%.40s", n->href);
        printf("\n");
    }
    for (int c = n->first_child; c >= 0; c = d->nodes[c].next_sibling)
        dump_dom(d, c, depth + 1);
}

/* One line per text run: position, size, resolved colour, resolved background,
 * and the text. This is the corpus's assertion surface -- it lets a page pin
 * what the CASCADE produced ("this word must be #e0604a") and not merely that
 * the word appeared, which is the difference between testing CSS and testing
 * that the parser did not crash. */
static void dump_text_runs(struct node_handle h, float ox, float oy, int inside) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float x = ox + n->tx, y = oy + n->ty;
    if (n->clip_children) inside = 1;
    if (inside && n->kind == SCENE_NODE_TEXT && n->data.text.utf8) {
        struct color c = n->data.text.color, b = n->data.text.bg;
        printf("RUN|%.1f|%.1f|%.1f|%.1f|#%02x%02x%02x|%s|%s\n", x, y, n->width, n->height,
               (unsigned)(c.r * 255.0f + 0.5f), (unsigned)(c.g * 255.0f + 0.5f),
               (unsigned)(c.b * 255.0f + 0.5f),
               b.a > 0.001f ? "bg" : "-", n->data.text.utf8);
    }
    for (struct node_handle c2 = n->first_child; !node_handle_is_null(c2);) {
        struct scene_node *cn = scene_resolve(&sa, c2);
        if (!cn) break;
        dump_text_runs(c2, x, y, inside);
        c2 = cn->next_sibling;
    }
}

static const char *kindname(enum scene_node_kind k) {
    switch (k) {
    case SCENE_NODE_GROUP: return "group";
    case SCENE_NODE_RECT:  return "rect ";
    case SCENE_NODE_IMAGE: return "image";
    case SCENE_NODE_TEXT:  return "TEXT ";
    }
    return "?";
}

/* Walk the SCENE tree after layout: every node already carries its resolved
 * size and its offset from its parent, so absolute position is just the sum
 * down the spine. */
static void dump(struct node_handle h, float ox, float oy, int depth, int maxdepth) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float x = ox + n->tx, y = oy + n->ty;

    if (depth <= maxdepth) {
        printf("%*s%s %7.1f,%-7.1f %6.1fx%-6.1f", depth * 2, "", kindname(n->kind), x, y,
               n->width, n->height);
        if (n->kind == SCENE_NODE_TEXT && n->data.text.utf8) {
            printf("  \"%s\"", n->data.text.utf8);
            if (n->data.text.bg.a > 0.001f) printf("  [BG a=%.2f]", n->data.text.bg.a);
        } else if (n->kind == SCENE_NODE_RECT) {
            const struct paint *f = &n->data.rect.fill;
            if (f->kind == PAINT_SOLID && f->solid.a > 0.001f)
                printf("  [fill %.2f,%.2f,%.2f a=%.2f]", f->solid.r, f->solid.g, f->solid.b, f->solid.a);
        }
        else if (n->clip_children)
            printf("  [clip]");
        printf("\n");
    }
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        dump(c, x, y, depth + 1, maxdepth);
        c = cn->next_sibling;
    }
}

/* The question this harness was built to answer: how many wrapping rows does
 * one paragraph become, and where does each one sit? A paragraph that renders
 * as three lines should be ONE row node whose children wrap inside it. If it
 * is three rows at three different depths, the fault is in what render.c
 * emits, not in how layout treats it. */
static void survey(struct node_handle h, float oy, int depth) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float y = oy + n->ty;
    /* a "line box" = a group with text children */
    int words = 0;
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        if (cn->kind == SCENE_NODE_TEXT) words++;
        c = cn->next_sibling;
    }
    if (words > 1)
        printf("  row y=%-7.1f h=%-6.1f words=%-3d depth=%d\n", y, n->height, words, depth);
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        survey(c, y, depth + 1);
        c = cn->next_sibling;
    }
}

/* Assertions that must HOLD, not merely print. `make browser-render` is the
 * fast loop for this whole stack, and a loop that always exits 0 is a report,
 * not a test. */
static int g_fail;

int main(int argc, char **argv) {
    const char *doc = argc > 1 ? argv[1] : "system/web/index.html";
    int W = argc > 2 ? atoi(argv[2]) : 940;
    int H = argc > 3 ? atoi(argv[3]) : 620;
    int maxdepth = argc > 4 ? atoi(argv[4]) : 6;
    const char *png = argc > 5 ? argv[5] : 0;
    g_busy = (argc > 6 && argv[6][0] == 'b');

    size_t rl = 0, bl = 0, dl = 0;
    uint8_t *reg  = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", &rl);
    uint8_t *bold = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", &bl);
    uint8_t *src  = read_file(doc, &dl);
    if (!reg || !bold) { fprintf(stderr, "could not load DejaVu fonts\n"); return 1; }
    if (!src) { fprintf(stderr, "could not read %s\n", doc); return 1; }
    uint32_t fr = font_load(reg, rl), fb = font_load(bold, bl);
    font_install_backend();

    vellum_set_link_handler(host_on_link);
    g_doc_base = doc;                /* before cssref_start: sheets resolve against it */
    g_root = html_parse(&g_doc, (const char *)src, dl, g_nodes, NODE_MAX, g_strs, STR_MAX);
    /* external sheets first, then the document's own <style> -- the same
     * order (and the same concatenation) the app builds its cascade with */
    /* the environment a media query is asked about -- the same content width
     * the document is laid out at, so `(min-width: N)` means what the page
     * thinks it means */
    css_media_set((float)W, (float)H, 1);
    /* ZOOM=1.5 renders the page as if the user had pressed + twice. The corpus
     * uses it to render one page at two zooms and compare the geometry, which
     * is the only way to test a scale: a single render at 1.0 would pass no
     * matter what zoom did. */
    if (getenv("ZOOM")) vellum_set_zoom((float)atof(getenv("ZOOM")));
    cssref_start(&g_doc, g_doc_base);
    {
        static char allcss[1024 * 1024];
        size_t n = 0, extn = 0;
        const char *ext = cssref_text(&extn);
        if (ext && extn) { memcpy(allcss, ext, extn); n = extn; allcss[n++] = '\n'; }
        if (g_doc.css && g_doc.css_len && n + g_doc.css_len < sizeof allcss - 1) {
            memcpy(allcss + n, g_doc.css, g_doc.css_len);
            n += g_doc.css_len;
        }
        allcss[n] = 0;
        css_sheet_parse(&g_sheet, n ? allcss : 0, n);
        vstyle_cache_invalidate();
    }
    /* Run the page's scripts, as the app does. Without this the harness tests
     * a DIFFERENT document than the browser renders -- any page that builds
     * its own DOM would look empty here and correct on the metal, which is the
     * exact divergence a fast loop exists to prevent. */
    if (jsdom_open(&g_doc, &g_sheet) == 0 && g_doc.n_js > 0) {
        int failed = jsdom_run_scripts();
        jsdom_take_dirty();
        vstyle_cache_invalidate();     /* the scripts may have restyled it */
        if (failed) printf("*** %d script(s) threw ***\n", failed);
    }
    if (getenv("DOM")) dump_dom(&g_doc, g_root, 0);
    /* HIDDEN=1 -- which elements the cascade turned off, and how much of the
     * document went with each one.
     *
     * A page that renders blank is almost always ONE element with display:none
     * too high up, and finding it by bisecting a stylesheet takes an evening.
     * This walks the tree, computes each element's style exactly as the
     * renderer does, and reports every subtree that gets dropped -- so the
     * question becomes a lookup. Shallowest first: the one nearest the root is
     * the one that matters. */
    /* GRID=1 -- every element that computes to display:grid, with the tracks
     * and named areas it got, and which of its children claimed one. "The
     * layout is stacked" is otherwise a guess about which of four things went
     * wrong. */
    /* TALL=1 -- the elements that own the page's height, biggest first.
     *
     * "The page is 300000 pixels long" is a fact about the whole document and
     * says nothing about which box did it. render.c keys every element's box
     * by its DOM node's address, so the instance tree can be walked back to
     * the element -- which turns a bisect into a list. */
    if (getenv("GRID")) {
        struct vstyle rs; vstyle_root(&rs);
        dump_grids(&g_doc, g_root, &rs, 0);
    }
    if (getenv("BUCKETS")) {
        int keyless = 0, byid = 0, bycls = 0, bytag = 0;
        for (int i = 0; i < g_sheet.n; i++) {
            const struct css_sel_part *j =
                &g_sheet.rules[i].sel.part[g_sheet.rules[i].sel.n - 1];
            if (j->id[0]) byid++;
            else if (j->klass[0]) bycls++;
            else if (j->tag[0]) bytag++;
            else {
                if (keyless < 6 && g_sheet.rules[i].decls) {
                    const char *d = g_sheet.rules[i].decls;
                    printf("KEYLESS| rule %d\n  ...before: %.110s\n  decls: %.70s\n",
                           i, d - 110, d);
                }
                keyless++;
            }
        }
        printf("BUCKETS| %d rules: id=%d class=%d tag=%d KEYLESS=%d\n",
               g_sheet.n, byid, bycls, bytag, keyless);
    }
    if (getenv("HIDDEN")) {
        struct vstyle rs; vstyle_root(&rs);
        dump_hidden(&g_doc, g_root, &rs, 0);
    }
    printf("%s: %zu bytes -> %d nodes%s, root %d, %d css rule%s%s\n", doc, dl, g_doc.n,
           g_doc.truncated ? " (TRUNCATED)" : "", g_root, g_sheet.n,
           g_sheet.n == 1 ? "" : "s", g_sheet.truncated ? " (TRUNCATED)" : "");
    if (g_root < 0) return 1;

    scene_arena_init(&sa);
    layout_arena_init(&la);
    ui_theme_use_dark(true);
    ui_theme_set_fonts(fr, fb);
    ui_init(&sa, &la);

    em_set_viewport((float)W, (float)H);
    if (getenv("TALL")) vellum_set_box_hook(box_hook);
    ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
    ui_run_layout((float)W, (float)H);

    /* AFTER layout: the whole point is how big things ENDED UP. It was
     * running before the frame was even built, which is a diagnostic that can
     * only ever report nothing. */
    if (getenv("TALL")) { g_tall_min = (float)atof(getenv("TALL")); dump_tall(&g_doc, &sa); }

    printf("\n--- resolved tree (depth <= %d) ---\n", maxdepth);
    dump(ui_scene_of(ui_root()), 0, 0, 0, maxdepth);

    printf("\n--- line boxes (a wrapped paragraph should be ONE row) ---\n");
    survey(ui_scene_of(ui_root()), 0, 0);

    /* --- scroll cost: N build+layout passes, exactly what a wheel tick costs.
     * The scroll offset changes each pass so this measures the SCROLLING case,
     * not a fully-static frame. --- */
    {
        struct timespec t0, t1;
        int N = 60;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        /* BUILD and LAYOUT timed apart. One number for both says a page is
         * slow; two say which half to go and look at, and they have been
         * different halves on different pages. */
        double build_ms = 0, layout_ms = 0;
        for (int i = 0; i < N; i++) {
            struct timespec a, b, c;
            g_scroll = (float)(i * 17 % 300);
            clock_gettime(CLOCK_MONOTONIC, &a);
            ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
            clock_gettime(CLOCK_MONOTONIC, &b);
            ui_run_layout((float)W, (float)H);
            clock_gettime(CLOCK_MONOTONIC, &c);
            build_ms  += (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
            layout_ms += (double)(c.tv_sec - b.tv_sec) * 1e3 + (double)(c.tv_nsec - b.tv_nsec) / 1e6;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / N;
        printf("\n--- scroll cost: %.2f ms per build+layout pass (host)"
               "  [build %.2f, layout %.2f] ---\n", ms, build_ms / N, layout_ms / N);
    }

    /* --- scroll-blit correctness: an INCREMENTAL scroll frame must be pixel-
     * identical to a from-scratch render at the same offset. The blit is an
     * optimization; if it can be told apart from the real thing, it is a bug.
     * Exercises the renderer's Step 1s exactly as a wheel tick does. --- */
    {
        int Wp = W, Hp = H;
        size_t nbytes = (size_t)Wp * Hp * 4;
        uint32_t *pa = malloc(nbytes), *pb = malloc(nbytes);
        const struct ui_theme *t = ui_theme();
        uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                    | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
        for (int i = 0; i < Wp * Hp; i++) { pa[i] = bg; pb[i] = bg; }
        struct render_target ta = { pa, (uint32_t)Wp, (uint32_t)Hp, (uint32_t)Wp * 4, EMBK_PIXFMT_BGRA8888_PRE };
        struct render_target tb = { pb, (uint32_t)Wp, (uint32_t)Hp, (uint32_t)Wp * 4, EMBK_PIXFMT_BGRA8888_PRE };

        struct scene_renderer ra; scene_render_init(&ra, cpu_backend_get());
        /* frame 1 at scroll 0, then frame 2 at scroll 64: the INCREMENTAL path */
        g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)Wp, (float)Hp);
        scene_render_frame(&ra, &sa, ui_scene_of(ui_root()), &ta);
        g_scroll = 64;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)Wp, (float)Hp);
        scene_render_frame(&ra, &sa, ui_scene_of(ui_root()), &ta);
        int blitted = ra.has_scroll_present;

        /* fresh renderer straight to scroll 64: the REFERENCE */
        struct scene_renderer rb; scene_render_init(&rb, cpu_backend_get());
        scene_render_frame(&rb, &sa, ui_scene_of(ui_root()), &tb);

        int diff = 0, dy0 = -1, dy1 = -1;
        for (int i = 0; i < Wp * Hp; i++) if (pa[i] != pb[i]) {
            diff++;
            int y = i / Wp;
            if (dy0 < 0) dy0 = y;
            dy1 = y;
        }
        if (diff) printf("    (differing rows: %d..%d)\n", dy0, dy1);
        printf("--- scroll blit: %s (blit path %s, %d differing px) ---\n",
               diff == 0 ? "PIXEL-EXACT" : "MISMATCH", blitted ? "TAKEN" : "not taken", diff);
        /* A check that passes because the code under test never ran is not a
         * check. This one read PIXEL-EXACT for as long as the classifier was
         * vetoing every real page -- the two renders agreed because they were
         * the same render. Not being taken is now a FAILURE. */
        /* A document shorter than its viewport cannot scroll, so the blit
         * genuinely does not apply -- say so rather than failing, or every
         * short page reports a bug in a feature it never invoked. */
        float maxy = 0; int nt = 0;
        text_extent(ui_scene_of(ui_root()), 0, 0, &maxy, &nt);
        int scrollable = maxy > (float)Hp;
        if (!scrollable) {
            printf("    (blit not applicable: the document fits its viewport)\n");
        } else if (!blitted || diff) {
            printf("*** scroll-blit check FAILED: %s ***\n",
                   !blitted ? "the blit path was never exercised" : "pixels differ");
            g_fail++;
        }
        scene_render_destroy(&ra); scene_render_destroy(&rb);
        free(pa); free(pb);
    }

    /* --- SELECTION: drag across the document, then read back what a copy
     * would put on the clipboard. The whole feature is post-layout logic over
     * the scene, so it runs here in full -- no window, no clipboard, no boot.
     * ------------------------------------------------------------------- */
    {
        HDEFER = 0; imgcache_reset(); g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        vsel_reset();
        vsel_sync_geometry();

        /* Select everything, which is also the Ctrl+A path. */
        int changed = vsel_all();
        static char buf[65536];
        size_t n = vsel_copy_text(buf, sizeof buf);
        int truncated = (n >= sizeof buf - 2);
        printf("\n--- selection: select-all changed=%d, %zu bytes copied ---\n", changed, n);
        if (!changed || n == 0) { printf("*** selection FAILED: select-all produced nothing ***\n"); g_fail++; }

        /* The copied text must be the DOCUMENT and not the chrome: the status
         * line and the address bar are text nodes too, and an earlier walk that
         * ignored the clip picked them up. */
        if (n && strstr(buf, g_bar)) {
            printf("*** selection FAILED: copied text contains the address bar ***\n");
            g_fail++;
        }
        /* ...and it must be in DOCUMENT ORDER, checked against the document
         * itself rather than against words hard-coded from one test page --
         * the first word copied has to be the document's first text node and
         * the last its last. Page-independent, so this runs on anything. */
        if (n) {
            printf("    first 200: [%.200s]\n", buf);
            struct node_handle first = NODE_HANDLE_NULL, last = NODE_HANDLE_NULL;
            first_last_text(ui_scene_of(ui_root()), 0, &first, &last);
            struct scene_node *fn = scene_resolve(&sa, first);
            struct scene_node *ln = scene_resolve(&sa, last);
            const char *fw = fn ? fn->data.text.utf8 : 0;
            const char *lw = ln ? ln->data.text.utf8 : 0;
            size_t flen = fw ? strlen(fw) : 0;
            while (flen && fw[flen-1] == ' ') flen--;
            int head_ok = fw && flen && !strncmp(buf, fw, flen);
            /* only when the copy FIT: a truncated copy is legitimately
             * missing its tail, and asserting otherwise tests the buffer */
            int tail_ok = truncated || (lw && lw[0] && strstr(buf, lw));
            if (!head_ok || !tail_ok) {
                printf("*** selection FAILED: copy is not the document in order"
                       " (head=%d tail=%d first=[%.30s] last=[%.30s]) ***\n",
                       head_ok, tail_ok, fw ? fw : "(none)", lw ? lw : "(none)");
                g_fail++;
            }
        }

        /* A drag between two points selects a SUBSET, and it must be smaller. */
        vsel_clear();
        vsel_pointer(60, 150, 1, 1);          /* press near the top of the page */
        vsel_pointer(400, 260, 0, 1);         /* drag down and right */
        static char sub[65536];
        size_t sn = vsel_copy_text(sub, sizeof sub);
        printf("--- selection: drag copied %zu bytes (of %zu) ---\n", sn, n);
        /* A drag must select something, and never more than everything. It is
         * only required to be a STRICT subset on a document long enough that
         * the drag rectangle cannot cover it -- on a three-line page, sweeping
         * from the first word to the last legitimately is the whole thing. */
        float dmaxy = 0; int dnt = 0;
        text_extent(ui_scene_of(ui_root()), 0, 0, &dmaxy, &dnt);
        if (sn == 0 || sn > n || (dmaxy > (float)H && sn >= n)) {
            printf("*** selection FAILED: drag selected %zu of %zu bytes ***\n", sn, n);
            g_fail++;
        }
        /* A press with no movement must select NOTHING, or every click on the
         * page would leave a word highlighted -- including a click on a link. */
        vsel_pointer(60, 150, 1, 1);
        if (vsel_active()) {
            printf("*** selection FAILED: a bare press selected something ***\n");
            g_fail++;
        }
        /* ...and UNPAINT. vsel_reset drops the range but the runs keep the
         * background they were last given -- in the app the post-layout pass
         * runs every frame and takes it away, here nothing would, and the PNG
         * below would show this test's highlight as if it were the page. */
        vsel_reset();
        vsel_sync_geometry();
    }

    /* --- THE REFLOW CHECK ------------------------------------------------
     * The claim images make is not "they render" -- it is that the page does
     * not MOVE when they land. So lay the document out twice, once with every
     * picture still outstanding and once with them all decoded, and compare
     * where the text ended up. Any difference is the reader's paragraph
     * jumping out from under them. --- */
    {
        float y_before = 0, y_after = 0;
        int   n_before = 0, n_after = 0;

        HDEFER = 1; imgcache_reset(); g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        text_extent(ui_scene_of(ui_root()), 0, 0, &y_before, &n_before);

        HDEFER = 0; imgcache_reset(); g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        text_extent(ui_scene_of(ui_root()), 0, 0, &y_after, &n_after);

        /* The verdict is Y, not the node count: an image with NO stated size
         * legitimately shows alt text until it arrives, so the counts differ
         * by design. What must not change is where the text sits. */
        printf("--- reflow: last text y=%.1f before images -> y=%.1f after : %s"
               "   (text nodes %d -> %d)%s ---\n",
               y_before, y_after,
               y_before == y_after ? "NO REFLOW" : "PAGE MOVED",
               n_before, n_after,
               n_before != n_after ? "  [unsized images fell back to alt text]" : "");
        /* Asserted only when the counts match. An image with NO stated size
         * cannot reserve its box, so it shows alt text and then reflows when
         * the real thing lands -- true here and true in every other browser,
         * which is what width/height are for. A differing count is exactly
         * that case, and failing on it would be asserting something the
         * browser never claimed. */
        if (n_before != n_after) {
            printf("    (reflow NOT asserted: an unsized image fell back to alt text)\n");
        } else if (y_before != y_after) {
            printf("*** reflow check FAILED: the page moved when images arrived ***\n");
            g_fail++;
        }
    }

    if (png) {
        /* Rebuild first. The checks above each leave the scene in whatever
         * state they needed -- a scroll offset, images deferred, a selection --
         * and the PNG was rendering THAT rather than the page. It showed text
         * from an earlier pass overlapping the current one, which reads as a
         * renderer bug and is not one. */
        HDEFER = 0; imgcache_reset(); g_scroll = 0; g_busy = 0;
        vsel_reset();
        /* Park the pointer off-screen. The checks above sweep it across the
         * page, and whatever it lands on renders HOVERED -- which put an
         * accent-filled box on one link of every page this harness drew and
         * looked exactly like a rendering bug. */
        em_feed_pointer(-10000.0f, -10000.0f, 0, 0, 0, 0);
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        /* ...and UNPAINT any selection. vsel_reset drops the RANGE; taking the
         * highlight off the runs is the post-layout pass's job, which the app
         * runs every frame and this harness has to run by hand. */
        vsel_sync_geometry();

        struct render_target rt;
        rt.pixels = malloc((size_t)W * H * 4); rt.width = W; rt.height = H;
        rt.stride = W * 4; rt.format = EMBK_PIXFMT_BGRA8888_PRE;
        const struct ui_theme *t = ui_theme();
        uint8_t br = (uint8_t)(t->bg.b * 255), bg = (uint8_t)(t->bg.g * 255),
                rr = (uint8_t)(t->bg.r * 255);
        for (int i = 0; i < W * H; i++)
            ((uint32_t *)rt.pixels)[i] = (255u << 24) | ((uint32_t)rr << 16) |
                                         ((uint32_t)bg << 8) | br;
        struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
        FILE *f = fopen(png, "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            uint32_t px = ((uint32_t *)rt.pixels)[i];
            uint8_t rgb[3] = { (uint8_t)((px >> 16) & 255), (uint8_t)((px >> 8) & 255),
                               (uint8_t)(px & 255) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        fprintf(stderr, "wrote %s (%dx%d)\n", png, W, H);
    }
    /* Did any view get dropped for want of an instance? A page that overflows
     * the pool renders INCOMPLETELY and silently -- collapsed boxes stacked at
     * one origin -- so it is a failure, not a footnote. */
    printf("--- instances: %u used, %u refused; layout containers overflowed: %d ---\n",
           ui_instance_used(), ui_instance_overflow(), layout_children_dropped());
    if (layout_children_dropped()) {
        printf("*** layout dropped children in %d container(s): boxes left at 0x0 ***\n",
               layout_children_dropped());
        g_fail++;
    }
    if (ui_instance_overflow()) {
        printf("*** instance pool OVERFLOWED: %u views dropped ***\n", ui_instance_overflow());
        g_fail++;
    }
    /* FIND=<text>: run find-in-page over the laid-out document and report what
     * it found. The count is the interesting part -- a matcher that spans runs
     * is exactly the kind that double-counts, and on a screenshot you only see
     * the highlight, never the number. */
    if (getenv("FIND")) {
        HDEFER = 0; imgcache_reset(); g_scroll = 0; g_busy = 0; vsel_reset();
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        vsel_sync_geometry();
        find_open();
        find_set_needle(getenv("FIND"));
        find_rescan();
        printf("FIND|%s|%d\n", getenv("FIND"), find_count());
        find_close();
    }
    if (getenv("TEXTDUMP")) {
        /* after every check, so the tree is the page as it finally renders */
        HDEFER = 0; imgcache_reset(); g_scroll = 0; g_busy = 0; vsel_reset();
        em_feed_pointer(-10000.0f, -10000.0f, 0, 0, 0, 0);
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        dump_text_runs(ui_scene_of(ui_root()), 0, 0, 0);
    }
    if (g_fail) fprintf(stderr, "\n*** browser-render: %d CHECK(S) FAILED ***\n", g_fail);
    return g_fail ? 1 : 0;
}

/* --- imgcache, host flavour --------------------------------------------- *
 * The on-target cache fetches over the network on a worker thread; here the
 * pictures are simply files. Same interface, so render.c is identical in both
 * worlds -- which is what makes the host render worth trusting. */
static struct img_slot H[IMG_SLOTS];
static uint32_t HARENA[IMG_MAX_PX];
static size_t   HUSED;
static uint8_t  HSCR[IMG_MAX_PX * 4 + IMG_MAX_DIM + 64];

/* --- external stylesheets, from disk ------------------------------------
 * The real cssref.c fetches over the network, which is the one thing this
 * harness cannot do. Loading the sheets from beside the document instead keeps
 * everything ABOVE the fetch -- resolution, concatenation, cascade order --
 * under test, which is where the interesting behaviour is. */
static char   HCSS[1024 * 1024];
static size_t HCSSN;

void cssref_reset(void) { HCSSN = 0; HCSS[0] = 0; }
int  cssref_pump(void) { return 0; }
int  cssref_pending(void) { return 0; }
int  cssref_dropped(void) { return 0; }
const char *cssref_text(size_t *len) { if (len) *len = HCSSN; return HCSSN ? HCSS : 0; }

int cssref_start(struct html_doc *doc, const char *base) {
    cssref_reset();
    if (!doc) return 0;
    int got = 0;
    for (int i = 0; i < doc->n_cssref; i++) {
        char path[1024];
        if (url_resolve(base, doc->cssref[i], path, sizeof path) != 0) continue;
        /* a network href has no file behind it here; skip rather than invent */
        size_t n = 0;
        uint8_t *buf = read_file(path, &n);
        if (!buf) { fprintf(stderr, "  (no local sheet for %s)\n", doc->cssref[i]); continue; }
        if (HCSSN + n + 2 < sizeof HCSS) {
            if (HCSSN) HCSS[HCSSN++] = '\n';
            memcpy(HCSS + HCSSN, buf, n);
            HCSSN += n;
            HCSS[HCSSN] = 0;
            got++;
        } else {
            fprintf(stderr, "  *** stylesheet %s DROPPED: %zu bytes would not fit ***\n",
                    doc->cssref[i], n);
        }
        free(buf);
    }
    if (got) fprintf(stderr, "  (loaded %d external stylesheet(s), %zu bytes)\n", got, HCSSN);
    return got;
}

/* --- the fetch worker, absent -------------------------------------------
 * jsdom's fetch() needs the worker; the harness has no network and does not
 * want one. A fetch here never STARTS, so a page's fetch promise simply never
 * settles -- which is honest (nothing was fetched) and keeps the JS engine,
 * the DOM bindings and the event model testable in two seconds, which is the
 * whole point of this file. */
int fetchjob_start(const char *url, char *buf, size_t cap, int tag) {
    (void)url; (void)buf; (void)cap; (void)tag; return -1;
}
int fetchjob_busy(void) { return 0; }
int fetchjob_poll(int tag, struct vnet_result *out) { (void)tag; (void)out; return -1; }

void imgcache_reset(void) { memset(H, 0, sizeof H); HUSED = 0; }
int  imgcache_pump(void) { return 0; }
int  imgcache_pending(void) { return 0; }

struct img_slot *imgcache_want(const char *url) {
    if (!url || !url[0]) return 0;
    if (HDEFER) {
        for (int i = 0; i < IMG_SLOTS; i++)
            if (H[i].state != IMG_EMPTY && !strcmp(H[i].url, url)) return &H[i];
        for (int i = 0; i < IMG_SLOTS; i++) {
            if (H[i].state != IMG_EMPTY) continue;
            snprintf(H[i].url, sizeof H[i].url, "%s", url);
            H[i].state = IMG_WANTED;
            return &H[i];
        }
        return 0;
    }
    for (int i = 0; i < IMG_SLOTS; i++)
        if (H[i].state != IMG_EMPTY && !strcmp(H[i].url, url)) return &H[i];
    for (int i = 0; i < IMG_SLOTS; i++) {
        if (H[i].state != IMG_EMPTY) continue;
        snprintf(H[i].url, sizeof H[i].url, "%s", url);
        size_t n = 0;
        /* AS GIVEN first, then repo-relative. The corpus lives in the repo and
         * writes "/system/web/photo.jpg", which is a path relative to the
         * checkout; a page fetched from the real web resolves its images to a
         * real absolute path, and stripping the leading slash turned that into
         * a relative one that does not exist. Every image on every real page
         * failed to load, and the pages rendered with grey boxes where the
         * pictures should be. */
        uint8_t *bytes = read_file(url, &n);
        if (!bytes) {
            const char *rel = url;
            while (*rel == '/') rel++;
            bytes = read_file(rel, &n);
        }
        if (!bytes) { H[i].state = IMG_FAILED; return &H[i]; }
        uint32_t w = 0, h = 0;
        int is_jpeg = n > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8;
        int probed = is_jpeg ? jpeg_probe(bytes, n, &w, &h) : png_probe(bytes, n, &w, &h);
        if (probed != 0 ||
            w > IMG_MAX_DIM || h > IMG_MAX_DIM || (size_t)w * h > IMG_MAX_PX) {
            H[i].state = IMG_FAILED; free(bytes); return &H[i];
        }
        uint32_t *dst = HARENA + HUSED;
        int drc = is_jpeg
            ? jpeg_decode(bytes, n, dst, (size_t)w * h * 4, HSCR, sizeof HSCR, &w, &h)
            : png_decode(bytes, n, dst, (size_t)w * h * 4, HSCR, sizeof HSCR, &w, &h);
        if (drc != 0) {
            H[i].state = IMG_FAILED; free(bytes); return &H[i];
        }
        HUSED += (size_t)w * h;
        H[i].px = dst; H[i].w = w; H[i].h = h; H[i].state = IMG_READY;
        free(bytes);
        return &H[i];
    }
    return 0;
}
