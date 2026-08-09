/* user/web/jsdom.c -- see jsdom.h.
 *
 * An element is exposed to JavaScript as a plain object carrying its NODE
 * INDEX, not a pointer. The DOM arena is an array that a mutation can grow, so
 * a pointer handed to a script is a pointer that a later appendChild could
 * invalidate -- and a script holding a stale element is a use-after-free with
 * a user's page as the trigger. An index cannot dangle; the worst it can do is
 * refer to a node that no longer exists, which every accessor checks.
 *
 * querySelector is nearly free here, and that is the payoff for having split
 * the CSS engine by concern: sel.c already answers "does this selector match
 * this element", so the binding is a walk plus a call.
 */
#include <string.h>
#include <stdio.h>

#include "quickjs.h"
#include "html.h"
#include "style.h"
#include "css.h"
#include "jsdom.h"
#include "fetchjob.h"
#include "net.h"
#include "form.h"
#include "url.h"
#include "cookie.h"
#include "store.h"

static JSRuntime *g_rt;
static JSContext *g_ctx;
static struct html_doc *g_doc;
static const struct css_sheet *g_sheet;
static const char *g_url;      /* the page's own address, for location */
static int  g_dirty;
static void (*g_console)(const char *line);
static int g_declined;        /* listeners for events we do not deliver */

/* ---- listeners + timers -------------------------------------------------
 * Fixed tables, like everything else that a stranger's page can grow. A page
 * that registers more than this gets the first N and keeps working, which is
 * the same bargain the DOM arena and the CSS rule table make.
 */
#define MAX_LISTENERS 64
#define MAX_TIMERS    32

/* An event TYPE, small enough to compare as an int. Only the ones this
 * browser can actually deliver exist -- registering a listener for an event
 * that will never fire is the hardest kind of bug to see, so an unknown name
 * is still refused loudly rather than accepted and forgotten. */
enum { EV_CLICK = 0, EV_SUBMIT, EV_INPUT, EV_CHANGE, EV_N };
static const char *const EV_NAME[EV_N] = { "click", "submit", "input", "change" };

static struct { int node; int type; JSValue fn; int used; } g_listen[MAX_LISTENERS];
static struct {
    int      id, used, repeat;
    unsigned long long due, every;
    JSValue  fn;
} g_timer[MAX_TIMERS];
static int g_timer_seq = 1;

/* ---- fetch --------------------------------------------------------------
 * A page's fetch shares the ONE worker the document and the images use, and
 * takes its turn behind them: the page you asked for outranks the data a
 * script wants about it. FETCH_TAG is how the poll knows the result is ours
 * (fetchjob.h -- a poll that does not own a result must not consume it).
 */
#define FETCH_TAG  3
#define FETCH_MAX  4
#define FETCH_BUF  (256 * 1024)

static struct {
    int     used, started;
    char    url[512];
    JSValue resolve, reject;
} g_fetch[FETCH_MAX];
static char g_fetch_buf[FETCH_BUF];
static int  g_fetch_active = -1;
static unsigned long long g_now;   /* last time the app pumped */

/* defined with the rest of the event machinery, below jsdom_open's globals */
static JSValue js_set_timeout(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_set_interval(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_clear_timer(JSContext *, JSValueConst, int, JSValueConst *);
/* Resolve href against base with the document's own resolver. Returns the
 * absolute string, or the href unchanged when it cannot be resolved -- a URL
 * that stays relative is wrong, but throwing here would take the whole script
 * down over a link nobody followed. */
static void jsdom_prelude(void);   /* defined with the prelude source below */

static JSValue js_noop(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx; (void)t; (void)argc; (void)argv; return JS_UNDEFINED;
}

static JSValue js_url_resolve(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    const char *href = argc > 0 ? JS_ToCString(ctx, argv[0]) : 0;
    const char *base = argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
                     ? JS_ToCString(ctx, argv[1]) : 0;
    char out[1024];
    JSValue r;
    if (href && url_resolve(base && base[0] ? base : (g_url ? g_url : ""),
                            href, out, sizeof out) == 0)
        r = JS_NewString(ctx, out);
    else
        r = JS_NewString(ctx, href ? href : "");
    if (href) JS_FreeCString(ctx, href);
    if (base) JS_FreeCString(ctx, base);
    return r;
}

static JSValue js_fetch(JSContext *, JSValueConst, int, JSValueConst *);

/* ---- element objects ---------------------------------------------------- */

static JSClassID g_elem_class;

/* The node index lives on the object; see the file header for why it is an
 * index and not a pointer. */
static int elem_index(JSContext *ctx, JSValueConst v) {
    JSValue p = JS_GetPropertyStr(ctx, v, "__i");
    int32_t i = -1;
    if (!JS_IsUndefined(p)) JS_ToInt32(ctx, &i, p);
    JS_FreeValue(ctx, p);
    if (!g_doc || i < 0 || i >= g_doc->n) return -1;
    return i;
}

static JSValue make_elem(JSContext *ctx, int idx);

/* textContent: reading concatenates the subtree's text, which is what the DOM
 * specifies and what a script asking for "the words in this element" means. */
static void gather_text(struct html_doc *d, int n, char *out, size_t cap, size_t *len) {
    if (n < 0 || n >= d->n) return;
    if (d->nodes[n].kind == HTML_TEXT) {
        const char *t = d->nodes[n].text;
        if (t) { size_t l = strlen(t);
                 if (*len + l < cap) { memcpy(out + *len, t, l); *len += l; } }
        return;
    }
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling)
        gather_text(d, c, out, cap, len);
}

static JSValue elem_get_text(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    static char buf[8192];
    size_t len = 0;
    gather_text(g_doc, i, buf, sizeof buf - 1, &len);
    buf[len] = 0;
    return JS_NewString(ctx, buf);
}

static JSValue elem_set_text(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (html_set_text(g_doc, i, s) == 0) g_dirty = 1;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue elem_get_attr(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const struct html_node *e = &g_doc->nodes[i];
    const char *v = 0;
    if      (!strcmp(name, "href") || !strcmp(name, "src")) v = e->href;
    else if (!strcmp(name, "class"))  v = e->klass;
    else if (!strcmp(name, "id"))     v = e->id;
    else if (!strcmp(name, "alt"))    v = e->alt;
    else if (!strcmp(name, "style"))  v = e->style;
    JS_FreeCString(ctx, name);
    /* The attributes this parser keeps are the ones something downstream can
     * ACT on (html.h §3). Anything else is genuinely absent, and null says so
     * rather than pretending the document did not have it. */
    return v ? JS_NewString(ctx, v) : JS_NULL;
}

static JSValue elem_get_tag(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    return JS_NewString(ctx, g_doc->nodes[i].tag);
}

/* Set an inline style declaration, e.g. el.setStyle("color:red"). Not the DOM's
 * `el.style.color = 'red'` -- that needs a property-per-CSS-property proxy
 * object, which is a lot of surface for the same effect. Named differently
 * BECAUSE it is different: a script author must not think they have the real
 * one and then wonder why `el.style.color` reads back undefined. */
static JSValue elem_set_style(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    /* borrow the document's own arena, exactly as the parser does */
    char *held = html_intern(g_doc, s, strlen(s));
    if (held) { g_doc->nodes[i].style = held; g_dirty = 1; }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* `this` is the event object; setting a flag on it is what the dispatch loop
 * reads back after the handler returns. */
static JSValue js_stop_propagation(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "__stop", JS_NewInt32(ctx, 1));
    return JS_UNDEFINED;
}

/* preventDefault. For `submit` this is a real veto: the browser asks after
 * dispatching, and does not navigate if a handler said no. That is what lets a
 * page validate a form, or handle the submission itself with fetch(), which is
 * how most forms on the modern web work. */
static JSValue js_prevent_default(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "__prevented", JS_NewInt32(ctx, 1));
    return JS_UNDEFINED;
}

static JSValue elem_add_listener(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_EXCEPTION;
    int ev = -1;
    for (int t = 0; t < EV_N; t++) if (!strcmp(type, EV_NAME[t])) { ev = t; break; }
    if (ev < 0) {
        /* NOT AN EXCEPTION. Refusing loudly was right when the scripts were
         * ours; on a real page it is fatal. `addEventListener('DOMContentLoaded')`
         * is the first line of a great many scripts, and throwing there
         * destroyed everything the script went on to do -- Brave's whole
         * bootstrap died on one listener it did not need us to deliver.
         *
         * So the listener is DECLINED, not registered: it will never fire, and
         * the console says which type was dropped so it is not silent either.
         * A listener that never runs is a hard bug to see; a page that stops
         * executing is a harder one. */
        /* COUNTED, NOT ANNOUNCED. This is a note for whoever is building the
         * browser, not for the person reading the page: they cannot act on it,
         * and it was crowding the status line off the screen on every page
         * that registers a DOMContentLoaded handler -- which is most of them.
         * jsdom_declined_listeners() hands the count to whoever wants it. */
        g_declined++;
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }
    JS_FreeCString(ctx, type);
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "listener must be a function");

    for (int k = 0; k < MAX_LISTENERS; k++) {
        if (g_listen[k].used) continue;
        g_listen[k].used = 1;
        g_listen[k].node = i;
        g_listen[k].type = ev;
        g_listen[k].fn = JS_DupValue(ctx, argv[1]);
        return JS_UNDEFINED;
    }
    return JS_ThrowInternalError(ctx, "too many listeners");
}

/* A control's value is the USER's, not the document's -- form.c holds it (see
 * form.h). Exposing it as a property is what lets a script validate, prefill
 * or clear a field, which is most of what page scripts do with forms. */
static JSValue elem_get_value(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    return JS_NewString(ctx, form_peek(i));
}
static JSValue elem_set_value(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    form_set(g_doc, i, s);
    g_dirty = 1;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* --- the document a script BUILDS ---------------------------------------- */

static JSValue elem_append(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    JSValue ci = JS_GetPropertyStr(ctx, argv[0], "__i");
    int c = -1; JS_ToInt32(ctx, &c, ci); JS_FreeValue(ctx, ci);
    if (c < 0) return JS_ThrowTypeError(ctx, "appendChild expects an element");
    if (html_append_child(g_doc, i, c) != 0)
        return JS_ThrowInternalError(ctx, "appendChild refused (cycle, or arena full)");
    g_dirty = 1;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue elem_remove_child(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    JSValue ci = JS_GetPropertyStr(ctx, argv[0], "__i");
    int c = -1; JS_ToInt32(ctx, &c, ci); JS_FreeValue(ctx, ci);
    if (c < 0 || g_doc->nodes[c].parent != i)
        return JS_ThrowTypeError(ctx, "removeChild: not a child of this node");
    html_remove_child(g_doc, c);
    g_dirty = 1;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue elem_remove(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    html_remove_child(g_doc, i);
    g_dirty = 1;
    return JS_UNDEFINED;
}

static JSValue elem_set_attr(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 2) return JS_UNDEFINED;
    const char *n = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    int rc = (n && v) ? html_set_attr(g_doc, i, n, v) : -1;
    if (n) JS_FreeCString(ctx, n);
    if (v) JS_FreeCString(ctx, v);
    if (rc == 0) g_dirty = 1;
    /* An attribute this browser does not store is REFUSED, not silently
     * accepted -- otherwise getAttribute reads back nothing and the script
     * blames itself. */
    if (rc != 0) return JS_ThrowTypeError(ctx, "setAttribute: unsupported attribute");
    return JS_UNDEFINED;
}

static JSValue elem_get_class(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || !g_doc->nodes[i].klass) return JS_NewString(ctx, "");
    return JS_NewString(ctx, g_doc->nodes[i].klass);
}

static JSValue elem_set_class(JSContext *ctx, JSValueConst this_val, JSValueConst v) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    if (html_set_attr(g_doc, i, "class", s) == 0) g_dirty = 1;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* class="a b c" -- does it contain `want`? */
static int class_has(const char *list, const char *want) {
    if (!list || !want || !*want) return 0;
    size_t wl = strlen(want);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *b = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - b) == wl && !strncmp(b, want, wl)) return 1;
    }
    return 0;
}

/* classList.add / remove / toggle / contains. A real classList is a live
 * object; this is the four methods every page actually calls, which is what
 * makes a class-driven UI work. */
static JSValue elem_class_op(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int op) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *cur = g_doc->nodes[i].klass ? g_doc->nodes[i].klass : "";
    int has = class_has(cur, name);
    if (op == 3) { JS_FreeCString(ctx, name); return JS_NewBool(ctx, has); }
    int add = (op == 0) || (op == 2 && !has);

    char buf[256]; size_t bn = 0;
    for (const char *p = cur; *p; ) {
        while (*p == ' ') p++;
        const char *b = p;
        while (*p && *p != ' ') p++;
        size_t l = (size_t)(p - b);
        if (!l) continue;
        if (l == strlen(name) && !strncmp(b, name, l)) continue;   /* drop it */
        if (bn && bn + 1 < sizeof buf) buf[bn++] = ' ';
        if (bn + l < sizeof buf) { memcpy(buf + bn, b, l); bn += l; }
    }
    if (add) {
        size_t l = strlen(name);
        if (bn && bn + 1 < sizeof buf) buf[bn++] = ' ';
        if (bn + l < sizeof buf) { memcpy(buf + bn, name, l); bn += l; }
    }
    buf[bn] = 0;
    if (html_set_attr(g_doc, i, "class", buf) == 0) g_dirty = 1;
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, add);
}
static JSValue elem_class_add(JSContext *c, JSValueConst t, int n, JSValueConst *a)
    { return elem_class_op(c, t, n, a, 0); }
static JSValue elem_class_rm(JSContext *c, JSValueConst t, int n, JSValueConst *a)
    { return elem_class_op(c, t, n, a, 1); }
static JSValue elem_class_tog(JSContext *c, JSValueConst t, int n, JSValueConst *a)
    { return elem_class_op(c, t, n, a, 2); }
static JSValue elem_class_has(JSContext *c, JSValueConst t, int n, JSValueConst *a)
    { return elem_class_op(c, t, n, a, 3); }

/* THE STANDARD SPELLINGS. `setText(x)` and `setValue(x)` are this DOM's own
 * names and no page on the web uses them: a real script writes
 * `el.textContent = "..."` and `el.value = x`. The methods stay (they are what
 * our own pages call), but the PROPERTIES are what a page actually assigns to,
 * and without a setter every one of those assignments silently did nothing --
 * or threw, once the script read back what it had just written. */
static JSValue elem_put_text(JSContext *ctx, JSValueConst this_val, JSValueConst v) {
    return elem_set_text(ctx, this_val, 1, &v);
}
static JSValue elem_put_value(JSContext *ctx, JSValueConst this_val, JSValueConst v) {
    return elem_set_value(ctx, this_val, 1, &v);
}


/* ---- tree navigation ----------------------------------------------------
 *
 * A DOM you can find a node in but not walk is half a DOM, and every script
 * that reached for `parentElement` -- which is most of them, since that is how
 * you get from the thing you matched to the thing you want to change -- died
 * on the property access before it ever ran. All of these are the same three
 * lines over html.h's index fields; they were simply never written.
 */
static int next_elem_sibling(int i) {
    for (int k = g_doc->nodes[i].next_sibling; k >= 0; k = g_doc->nodes[k].next_sibling)
        if (g_doc->nodes[k].kind == HTML_ELEM) return k;
    return -1;
}
static int prev_elem_sibling(int i) {
    int p = g_doc->nodes[i].parent, last = -1;
    if (p < 0) return -1;
    for (int k = g_doc->nodes[p].first_child; k >= 0 && k != i; k = g_doc->nodes[k].next_sibling)
        if (g_doc->nodes[k].kind == HTML_ELEM) last = k;
    return last;
}

static JSValue elem_get_parent(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t);
    if (i < 0) return JS_NULL;
    int p = g_doc->nodes[i].parent;
    return p >= 0 ? make_elem(ctx, p) : JS_NULL;
}
static JSValue elem_get_children(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t);
    JSValue a = JS_NewArray(ctx);
    if (i < 0) return a;
    uint32_t n = 0;
    for (int k = g_doc->nodes[i].first_child; k >= 0; k = g_doc->nodes[k].next_sibling)
        if (g_doc->nodes[k].kind == HTML_ELEM)
            JS_SetPropertyUint32(ctx, a, n++, make_elem(ctx, k));
    return a;
}
static JSValue elem_get_first_child(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t);
    if (i < 0) return JS_NULL;
    for (int k = g_doc->nodes[i].first_child; k >= 0; k = g_doc->nodes[k].next_sibling)
        if (g_doc->nodes[k].kind == HTML_ELEM) return make_elem(ctx, k);
    return JS_NULL;
}
static JSValue elem_get_last_child(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t), last = -1;
    if (i < 0) return JS_NULL;
    for (int k = g_doc->nodes[i].first_child; k >= 0; k = g_doc->nodes[k].next_sibling)
        if (g_doc->nodes[k].kind == HTML_ELEM) last = k;
    return last >= 0 ? make_elem(ctx, last) : JS_NULL;
}
static JSValue elem_get_next(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t);
    if (i < 0) return JS_NULL;
    int k = next_elem_sibling(i);
    return k >= 0 ? make_elem(ctx, k) : JS_NULL;
}
static JSValue elem_get_prev(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t);
    if (i < 0) return JS_NULL;
    int k = prev_elem_sibling(i);
    return k >= 0 ? make_elem(ctx, k) : JS_NULL;
}
static JSValue elem_get_id(JSContext *ctx, JSValueConst t) {
    int i = elem_index(ctx, t);
    if (i < 0) return JS_NewString(ctx, "");
    const char *v = g_doc->nodes[i].id;
    return JS_NewString(ctx, v ? v : "");
}

/* insertBefore(newNode, refNode) -- append when ref is null, which is what the
 * DOM says and what half the callers rely on. */
static JSValue elem_insert_before(JSContext *ctx, JSValueConst t,
                                  int argc, JSValueConst *argv) {
    int p = elem_index(ctx, t);
    int c = argc > 0 ? elem_index(ctx, argv[0]) : -1;
    if (p < 0 || c < 0) return JS_UNDEFINED;
    int ref = (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
            ? elem_index(ctx, argv[1]) : -1;
    if (ref < 0 || g_doc->nodes[ref].parent != p) {
        html_append_child(g_doc, p, c);
        g_dirty = 1;
        return JS_DupValue(ctx, argv[0]);
    }
    /* Append first so the node is detached from wherever it was, then move it
     * into place -- the list is singly linked, so this is a relink, not a
     * shuffle. */
    html_append_child(g_doc, p, c);
    int prev = -1;
    for (int k = g_doc->nodes[p].first_child; k >= 0; k = g_doc->nodes[k].next_sibling) {
        if (k == c) break;
        prev = k;
    }
    if (prev >= 0) g_doc->nodes[prev].next_sibling = g_doc->nodes[c].next_sibling;
    else           g_doc->nodes[p].first_child     = g_doc->nodes[c].next_sibling;
    prev = -1;
    for (int k = g_doc->nodes[p].first_child; k >= 0; k = g_doc->nodes[k].next_sibling) {
        if (k == ref) break;
        prev = k;
    }
    g_doc->nodes[c].next_sibling = ref;
    if (prev >= 0) g_doc->nodes[prev].next_sibling = c;
    else           g_doc->nodes[p].first_child     = c;
    g_dirty = 1;
    return JS_DupValue(ctx, argv[0]);
}

/* querySelector scoped to a subtree: `el.querySelector(...)` is as common as
 * the document-level one, and a page that has one container and searches
 * inside it uses nothing else. */
static JSValue elem_query(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);
static JSValue elem_query_all(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

static const JSCFunctionListEntry elem_proto[] = {
    JS_CGETSET_DEF("value", elem_get_value, elem_put_value),
    JS_CFUNC_DEF("setValue", 1, elem_set_value),
    JS_CFUNC_DEF("addEventListener", 2, elem_add_listener),
    JS_CGETSET_DEF("textContent", elem_get_text, elem_put_text),
    /* innerText is textContent's other name; pages use both. */
    JS_CGETSET_DEF("innerText", elem_get_text, elem_put_text),
    JS_CGETSET_DEF("tagName", elem_get_tag, 0),
    JS_CFUNC_DEF("setText", 1, elem_set_text),
    JS_CFUNC_DEF("getAttribute", 1, elem_get_attr),
    JS_CFUNC_DEF("setStyle", 1, elem_set_style),
    JS_CFUNC_DEF("appendChild", 1, elem_append),
    JS_CFUNC_DEF("removeChild", 1, elem_remove_child),
    JS_CFUNC_DEF("remove", 0, elem_remove),
    JS_CFUNC_DEF("setAttribute", 2, elem_set_attr),
    JS_CGETSET_DEF("className", elem_get_class, elem_set_class),
    JS_CFUNC_DEF("classAdd", 1, elem_class_add),
    JS_CFUNC_DEF("classRemove", 1, elem_class_rm),
    JS_CFUNC_DEF("classToggle", 1, elem_class_tog),
    JS_CFUNC_DEF("classContains", 1, elem_class_has),
    JS_CGETSET_DEF("parentElement", elem_get_parent, 0),
    JS_CGETSET_DEF("parentNode", elem_get_parent, 0),
    JS_CGETSET_DEF("children", elem_get_children, 0),
    JS_CGETSET_DEF("childNodes", elem_get_children, 0),
    JS_CGETSET_DEF("firstElementChild", elem_get_first_child, 0),
    JS_CGETSET_DEF("firstChild", elem_get_first_child, 0),
    JS_CGETSET_DEF("lastElementChild", elem_get_last_child, 0),
    JS_CGETSET_DEF("lastChild", elem_get_last_child, 0),
    JS_CGETSET_DEF("nextElementSibling", elem_get_next, 0),
    JS_CGETSET_DEF("nextSibling", elem_get_next, 0),
    JS_CGETSET_DEF("previousElementSibling", elem_get_prev, 0),
    JS_CGETSET_DEF("previousSibling", elem_get_prev, 0),
    JS_CGETSET_DEF("id", elem_get_id, 0),
    JS_CFUNC_DEF("insertBefore", 2, elem_insert_before),
    JS_CFUNC_DEF("querySelector", 1, elem_query),
    JS_CFUNC_DEF("querySelectorAll", 1, elem_query_all),
};

static JSValue make_elem(JSContext *ctx, int idx);

/* The host and path of the page, for cookie scoping. A local file has no
 * host, and a cookie without one belongs to nothing -- so file:// pages get an
 * empty jar rather than a shared one. */
static void page_origin(char *host, size_t hcap, char *path, size_t pcap) {
    struct url u;
    host[0] = 0; snprintf(path, pcap, "/");
    if (!g_url || url_parse(g_url, &u) != 0) return;
    if (u.kind == URL_LOCAL) return;
    snprintf(host, hcap, "%s", u.host);
    snprintf(path, pcap, "%s", u.path[0] ? u.path : "/");
}

static JSValue doc_get_cookie(JSContext *ctx, JSValueConst this_val) {
    (void)this_val;
    char host[128], path[256], out[1024];
    page_origin(host, sizeof host, path, sizeof path);
    if (!host[0]) return JS_NewString(ctx, "");
    cookie_for_script(host, path, out, sizeof out);
    return JS_NewString(ctx, out);
}

static JSValue doc_set_cookie(JSContext *ctx, JSValueConst this_val, JSValueConst v) {
    (void)this_val;
    char host[128], path[256];
    page_origin(host, sizeof host, path, sizeof path);
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    if (host[0]) cookie_set(host, s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* localStorage. Scoped to the page's ORIGIN, so one site cannot read another's
 * -- which is the entire contract, and the reason this is not one global map.
 * Persisted on every write: a page that stores something and then navigates
 * must find it again, and there is no unload event here to flush on. */
static void ls_origin(char *out, size_t cap) {
    struct url u;
    out[0] = 0;
    if (!g_url || url_parse(g_url, &u) != 0) return;
    /* A local file gets the empty origin: its own private area rather than one
     * shared with every other file on the disk. */
    if (u.kind != URL_LOCAL) snprintf(out, cap, "%s", u.host);
}

static JSValue ls_get(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NULL;
    char o[128]; ls_origin(o, sizeof o);
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_EXCEPTION;
    const char *v = store_get(o, k);
    JS_FreeCString(ctx, k);
    return v ? JS_NewString(ctx, v) : JS_NULL;
}

static JSValue ls_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    char o[128]; ls_origin(o, sizeof o);
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    int rc = (k && v) ? store_set(o, k, v) : -1;
    if (k) JS_FreeCString(ctx, k);
    if (v) JS_FreeCString(ctx, v);
    if (rc != 0) return JS_ThrowInternalError(ctx, "localStorage is full");
    store_save();
    return JS_UNDEFINED;
}

static JSValue ls_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    char o[128]; ls_origin(o, sizeof o);
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_EXCEPTION;
    store_remove(o, k);
    JS_FreeCString(ctx, k);
    store_save();
    return JS_UNDEFINED;
}

static JSValue ls_clear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx; (void)t; (void)argc; (void)argv;
    char o[128]; ls_origin(o, sizeof o);
    store_clear(o);
    store_save();
    return JS_UNDEFINED;
}

static JSValue ls_key(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NULL;
    char o[128]; ls_origin(o, sizeof o);
    int i = 0; JS_ToInt32(ctx, &i, argv[0]);
    const char *k = store_key_at(o, i);
    return k ? JS_NewString(ctx, k) : JS_NULL;
}

static JSValue ls_length(JSContext *ctx, JSValueConst t) {
    (void)t;
    char o[128]; ls_origin(o, sizeof o);
    return JS_NewInt32(ctx, store_count(o));
}

static JSValue doc_create_element(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_EXCEPTION;
    int i = html_create_element(g_doc, tag);
    JS_FreeCString(ctx, tag);
    if (i < 0) return JS_ThrowInternalError(ctx, "document node arena is full");
    /* DETACHED, as the DOM says: a script builds a subtree and appends it when
     * it is ready, which is what every one of them does. */
    return make_elem(ctx, i);
}

static JSValue doc_create_text(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *t = JS_ToCString(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    int i = html_create_text(g_doc, t);
    JS_FreeCString(ctx, t);
    if (i < 0) return JS_ThrowInternalError(ctx, "document string arena is full");
    return make_elem(ctx, i);
}

static JSValue make_elem(JSContext *ctx, int idx) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__i", JS_NewInt32(ctx, idx));
    JS_SetPropertyFunctionList(ctx, o, elem_proto,
                               sizeof elem_proto / sizeof elem_proto[0]);
    /* classList as a small object over the same element. The real one is live
     * and iterable; these are the four methods pages actually call. */
    JSValue cl = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cl, "__i", JS_NewInt32(ctx, idx));
    JS_SetPropertyStr(ctx, cl, "add",
        JS_NewCFunction(ctx, elem_class_add, "add", 1));
    JS_SetPropertyStr(ctx, cl, "remove",
        JS_NewCFunction(ctx, elem_class_rm, "remove", 1));
    JS_SetPropertyStr(ctx, cl, "toggle",
        JS_NewCFunction(ctx, elem_class_tog, "toggle", 1));
    JS_SetPropertyStr(ctx, cl, "contains",
        JS_NewCFunction(ctx, elem_class_has, "contains", 1));
    JS_SetPropertyStr(ctx, o, "classList", cl);
    return o;
}

/* ---- document ----------------------------------------------------------- */

/* Depth-first, document order -- the order querySelectorAll must return, and
 * the order a reader would find them in. */
static int find_match(struct html_doc *d, int n, const struct css_sel *sel,
                      int *out, int max, int *count) {
    if (n < 0 || n >= d->n) return 0;
    if (d->nodes[n].kind == HTML_ELEM && css_sel_match(sel, d, n)) {
        if (out && *count < max) out[*count] = n;
        (*count)++;
        if (!out) return n;                      /* querySelector: first wins */
    }
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        int r = find_match(d, c, sel, out, max, count);
        if (!out && r >= 0) return r;
    }
    return -1;
}


/* Element-scoped search: the same walk, rooted at the element and skipping the
 * element itself -- `el.querySelector('.x')` must not match `el`. Pages that
 * grab one container and then search inside it use nothing else. */
static JSValue elem_query(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int i = elem_index(ctx, t);
    if (i < 0 || argc < 1) return JS_NULL;
    const char *sn = JS_ToCString(ctx, argv[0]);
    if (!sn) return JS_EXCEPTION;
    struct css_sel sel;
    int ok = css_sel_parse(sn, strlen(sn), &sel) == 0;
    JS_FreeCString(ctx, sn);
    if (!ok) return JS_NULL;
    int count = 0;
    for (int c = g_doc->nodes[i].first_child; c >= 0; c = g_doc->nodes[c].next_sibling) {
        int hit = find_match(g_doc, c, &sel, 0, 0, &count);
        if (hit >= 0) return make_elem(ctx, hit);
    }
    return JS_NULL;
}

static JSValue elem_query_all(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int i = elem_index(ctx, t);
    JSValue arr = JS_NewArray(ctx);
    if (i < 0 || argc < 1) return arr;
    const char *sn = JS_ToCString(ctx, argv[0]);
    if (!sn) return arr;
    struct css_sel sel;
    int ok = css_sel_parse(sn, strlen(sn), &sel) == 0;
    JS_FreeCString(ctx, sn);
    if (!ok) return arr;
    enum { QMAX = 256 };
    int hits[QMAX], count = 0;
    for (int c = g_doc->nodes[i].first_child; c >= 0; c = g_doc->nodes[c].next_sibling)
        find_match(g_doc, c, &sel, hits, QMAX, &count);
    int n = count < QMAX ? count : QMAX;
    for (int k = 0; k < n; k++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)k, make_elem(ctx, hits[k]));
    return arr;
}

static JSValue doc_query(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !g_doc) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    struct css_sel sel;
    int ok = css_sel_parse(s, strlen(s), &sel) == 0;
    JS_FreeCString(ctx, s);
    if (!ok) return JS_NULL;
    int count = 0;
    int hit = find_match(g_doc, g_doc->root, &sel, 0, 0, &count);
    return hit >= 0 ? make_elem(ctx, hit) : JS_NULL;
}

static JSValue doc_query_all(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (argc < 1 || !g_doc) return arr;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return arr;
    struct css_sel sel;
    int ok = css_sel_parse(s, strlen(s), &sel) == 0;
    JS_FreeCString(ctx, s);
    if (!ok) return arr;
    static int hits[256];
    int count = 0;
    find_match(g_doc, g_doc->root, &sel, hits, 256, &count);
    if (count > 256) count = 256;
    for (int i = 0; i < count; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, make_elem(ctx, hits[i]));
    return arr;
}

static JSValue doc_get_title(JSContext *ctx, JSValueConst this_val) {
    (void)this_val;
    if (!g_doc) return JS_UNDEFINED;
    struct css_sel sel;
    if (css_sel_parse("title", 5, &sel) != 0) return JS_UNDEFINED;
    int count = 0;
    int t = find_match(g_doc, g_doc->root, &sel, 0, 0, &count);
    if (t < 0) return JS_NewString(ctx, "");
    static char buf[512]; size_t len = 0;
    gather_text(g_doc, t, buf, sizeof buf - 1, &len);
    buf[len] = 0;
    return JS_NewString(ctx, buf);
}

/* ---- console ------------------------------------------------------------ */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    static char line[1024];
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        int w = snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", s);
        JS_FreeCString(ctx, s);
        if (w > 0) n += (size_t)w;
        if (n >= sizeof line - 1) break;
    }
    line[n < sizeof line ? n : sizeof line - 1] = 0;
    if (g_console) g_console(line);
    return JS_UNDEFINED;
}

/* ---- lifecycle ---------------------------------------------------------- */

void jsdom_set_console(void (*fn)(const char *line)) { g_console = fn; }
int  jsdom_declined_listeners(void) { return g_declined; }
int  jsdom_take_dirty(void) { int d = g_dirty; g_dirty = 0; return d; }

void jsdom_close(void) {
    if (g_ctx) {
        for (int k = 0; k < MAX_LISTENERS; k++)
            if (g_listen[k].used) { JS_FreeValue(g_ctx, g_listen[k].fn); g_listen[k].used = 0; }
        for (int k = 0; k < MAX_TIMERS; k++)
            if (g_timer[k].used) { JS_FreeValue(g_ctx, g_timer[k].fn); g_timer[k].used = 0; }
        /* An unsettled promise whose page is gone is not an error to report --
         * there is no longer anyone to report it to. Drop the handlers. */
        for (int k = 0; k < FETCH_MAX; k++)
            if (g_fetch[k].used) {
                JS_FreeValue(g_ctx, g_fetch[k].resolve);
                JS_FreeValue(g_ctx, g_fetch[k].reject);
                g_fetch[k].used = 0;
            }
        g_fetch_active = -1;
    }
    if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = 0; }
    if (g_rt)  { JS_FreeRuntime(g_rt);  g_rt = 0; }
    g_doc = 0; g_sheet = 0; g_dirty = 0;
}

void jsdom_set_url(const char *url) { g_url = url; }

int jsdom_open(struct html_doc *doc, const struct css_sheet *sheet) {
    jsdom_close();
    g_doc = doc; g_sheet = sheet;
    g_rt = JS_NewRuntime();
    if (!g_rt) return -1;
    /* A page's script gets a BUDGET. An accidental `while(1)` on a stranger's
     * page must not take the window with it, and a browser that can be hung by
     * one line of someone else's JavaScript is not one you can browse with. */
    JS_SetMemoryLimit(g_rt, 16u * 1024 * 1024);
    JS_SetMaxStackSize(g_rt, 512u * 1024);
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) { JS_FreeRuntime(g_rt); g_rt = 0; return -1; }

    JSValue g = JS_GetGlobalObject(g_ctx);

    JSValue console = JS_NewObject(g_ctx);
    /* log was the ONLY method, so `console.warn(...)` -- which is what a page
     * writes in the catch block it wrapped around a feature it expected to be
     * missing -- threw inside the handler and took the script down anyway. The
     * whole family goes to the same place; a browser console distinguishes
     * them by colour, and we do not have colours. */
    static const char *const CONSOLE_FNS[] = {
        "log", "warn", "error", "info", "debug", "trace", "dir", 0
    };
    for (int ci2 = 0; CONSOLE_FNS[ci2]; ci2++)
        JS_SetPropertyStr(g_ctx, console, CONSOLE_FNS[ci2],
                          JS_NewCFunction(g_ctx, js_console_log, CONSOLE_FNS[ci2], 1));
    /* ...and the no-ops a page calls for timing and grouping. Absent, they
     * throw; present and silent, the page carries on. */
    {
        static const char *const NOOPS[] = { "group", "groupEnd", "time",
                                             "timeEnd", "count", "assert", 0 };
        for (int ci2 = 0; NOOPS[ci2]; ci2++)
            JS_SetPropertyStr(g_ctx, console, NOOPS[ci2],
                              JS_NewCFunction(g_ctx, js_noop, NOOPS[ci2], 0));
    }
    JS_SetPropertyStr(g_ctx, g, "console", console);

    {
        JSValue ls = JS_NewObject(g_ctx);
        JS_SetPropertyStr(g_ctx, ls, "getItem",    JS_NewCFunction(g_ctx, ls_get, "getItem", 1));
        JS_SetPropertyStr(g_ctx, ls, "setItem",    JS_NewCFunction(g_ctx, ls_set, "setItem", 2));
        JS_SetPropertyStr(g_ctx, ls, "removeItem", JS_NewCFunction(g_ctx, ls_remove, "removeItem", 1));
        JS_SetPropertyStr(g_ctx, ls, "clear",      JS_NewCFunction(g_ctx, ls_clear, "clear", 0));
        JS_SetPropertyStr(g_ctx, ls, "key",        JS_NewCFunction(g_ctx, ls_key, "key", 1));
        JSAtom la = JS_NewAtom(g_ctx, "length");
        JS_DefinePropertyGetSet(g_ctx, ls, la,
            JS_NewCFunction(g_ctx, (JSCFunction *)ls_length, "length", 0),
            JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(g_ctx, la);
        JS_SetPropertyStr(g_ctx, g, "localStorage", ls);
    }

    JSValue d = JS_NewObject(g_ctx);
    /* document.write: a NO-OP that does not throw.
     *
     * Nearly every use left on the web is the feature-detect fallback --
     * `window.jQuery || document.write('<script src=...>')` -- and since we do
     * not fetch external scripts there is nothing useful to do with it. But
     * ABSENT it threw a TypeError, which killed the script at that line and
     * took everything after it with it; present and silent, the page carries on
     * exactly as it would with a script that failed to load. Writing document
     * CONTENT this way would need the parser to splice a fragment mid-parse,
     * which is a different feature and is logged as one. */
    JS_SetPropertyStr(g_ctx, d, "write",
                      JS_NewCFunction(g_ctx, js_noop, "write", 1));
    JS_SetPropertyStr(g_ctx, d, "writeln",
                      JS_NewCFunction(g_ctx, js_noop, "writeln", 1));
    JS_SetPropertyStr(g_ctx, d, "querySelector",
                      JS_NewCFunction(g_ctx, doc_query, "querySelector", 1));
    JS_SetPropertyStr(g_ctx, d, "querySelectorAll",
                      JS_NewCFunction(g_ctx, doc_query_all, "querySelectorAll", 1));
    JS_SetPropertyStr(g_ctx, d, "createElement",
                      JS_NewCFunction(g_ctx, doc_create_element, "createElement", 1));
    JS_SetPropertyStr(g_ctx, d, "createTextNode",
                      JS_NewCFunction(g_ctx, doc_create_text, "createTextNode", 1));
    /* document.body: the thing a script appends to. Resolved lazily by tag so
     * a document without an explicit <body> still answers with its root. */
    {
        int b = -1;
        for (int i = 0; i < g_doc->n && b < 0; i++)
            if (g_doc->nodes[i].kind == HTML_ELEM && !strcmp(g_doc->nodes[i].tag, "body")) b = i;
        if (b < 0) b = 0;
        JS_SetPropertyStr(g_ctx, d, "body", make_elem(g_ctx, b));
    }
    JSAtom t = JS_NewAtom(g_ctx, "title");
    JS_DefinePropertyGetSet(g_ctx, d, t,
                            JS_NewCFunction(g_ctx, (JSCFunction *)doc_get_title, "title", 0),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(g_ctx, t);
    /* document.cookie. HttpOnly cookies are EXCLUDED, which is the entire
     * security value of that flag -- a script that could read them would make
     * it decorative. */
    {
        JSAtom ca = JS_NewAtom(g_ctx, "cookie");
        JS_DefinePropertyGetSet(g_ctx, d, ca,
            JS_NewCFunction(g_ctx, (JSCFunction *)doc_get_cookie, "cookie", 0),
            JS_NewCFunction(g_ctx, (JSCFunction *)doc_set_cookie, "cookie", 1),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(g_ctx, ca);
    }

    /* location: a page that cannot read its own URL cannot act on a form's
     * query string, which is how half the web's search results pages work. */
    JSValue loc = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, loc, "href", JS_NewString(g_ctx, g_url ? g_url : ""));
    {
        struct url u;
        if (g_url && url_parse(g_url, &u) == 0) {
            JS_SetPropertyStr(g_ctx, loc, "pathname", JS_NewString(g_ctx, u.path));
            char q[300];
            snprintf(q, sizeof q, "%s%s", u.query[0] ? "?" : "", u.query);
            JS_SetPropertyStr(g_ctx, loc, "search", JS_NewString(g_ctx, q));
        } else {
            JS_SetPropertyStr(g_ctx, loc, "pathname", JS_NewString(g_ctx, ""));
            JS_SetPropertyStr(g_ctx, loc, "search", JS_NewString(g_ctx, ""));
        }
    }
    JS_SetPropertyStr(g_ctx, g, "location", loc);

    /* `window` IS the global object. Scripts written for a browser say
     * `window.addEventListener` and `window.location` far more often than they
     * say the bare name, and every one of them threw a ReferenceError. */
    JS_SetPropertyStr(g_ctx, g, "window", JS_DupValue(g_ctx, g));

    /* The one primitive the URL class cannot do in script: resolution against
     * a base. url.c already does it, is tested on the host, and is what every
     * link on a page goes through -- so the class calls THAT rather than
     * carrying a second, differently-wrong implementation in JavaScript. */
    JS_SetPropertyStr(g_ctx, g, "__url_resolve",
                      JS_NewCFunction(g_ctx, js_url_resolve, "__url_resolve", 2));

    JS_SetPropertyStr(g_ctx, g, "document", d);

    JS_SetPropertyStr(g_ctx, g, "setTimeout",
                      JS_NewCFunction(g_ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(g_ctx, g, "setInterval",
                      JS_NewCFunction(g_ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(g_ctx, g, "clearTimeout",
                      JS_NewCFunction(g_ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(g_ctx, g, "clearInterval",
                      JS_NewCFunction(g_ctx, js_clear_timer, "clearInterval", 1));
    JS_SetPropertyStr(g_ctx, g, "fetch",
                      JS_NewCFunction(g_ctx, js_fetch, "fetch", 1));

    /* ...and the script-level globals built on top of them. */
    jsdom_prelude();

    JS_FreeValue(g_ctx, g);
    (void)g_elem_class;
    return 0;
}

/* THE PRELUDE: the handful of globals every modern page assumes exist.
 *
 * Written in JavaScript rather than as C bindings because it is all string
 * work over one primitive (__url_resolve) that already exists and is tested --
 * a second URL parser in C would be more code and a second thing to get wrong.
 * Deliberately not the whole spec: this is what pages actually touch.
 */
static const char JSDOM_PRELUDE[] =
"(function(){\n"
"var RE=/^(?:([A-Za-z][A-Za-z0-9+.-]*:))?(?:\\/\\/([^\\/?#]*))?([^?#]*)(\\?[^#]*)?(#.*)?$/;\n"
"function SP(init){ this._p=[];\n"
"  if(init && typeof init==='object' && init._p){ for(var i=0;i<init._p.length;i++) this._p.push([init._p[i][0],init._p[i][1]]); return; }\n"
"  var s=(init==null?'':String(init)); if(s.charAt(0)==='?') s=s.slice(1);\n"
"  if(!s) return;\n"
"  var parts=s.split('&');\n"
"  for(var i=0;i<parts.length;i++){ if(!parts[i]) continue;\n"
"    var e=parts[i].indexOf('='), k=e<0?parts[i]:parts[i].slice(0,e), v=e<0?'':parts[i].slice(e+1);\n"
"    this._p.push([dec(k),dec(v)]); } }\n"
"function dec(x){ try{ return decodeURIComponent(String(x).replace(/\\+/g,' ')); }catch(_){ return String(x); } }\n"
"function enc(x){ try{ return encodeURIComponent(String(x)); }catch(_){ return String(x); } }\n"
"SP.prototype.get=function(k){ k=String(k); for(var i=0;i<this._p.length;i++) if(this._p[i][0]===k) return this._p[i][1]; return null; };\n"
"SP.prototype.getAll=function(k){ k=String(k); var r=[]; for(var i=0;i<this._p.length;i++) if(this._p[i][0]===k) r.push(this._p[i][1]); return r; };\n"
"SP.prototype.has=function(k){ return this.get(k)!==null; };\n"
"SP.prototype.append=function(k,v){ this._p.push([String(k),String(v)]); };\n"
"SP.prototype.set=function(k,v){ k=String(k); for(var i=0;i<this._p.length;i++) if(this._p[i][0]===k){ this._p[i][1]=String(v); for(var j=this._p.length-1;j>i;j--) if(this._p[j][0]===k) this._p.splice(j,1); return; } this.append(k,v); };\n"
"SP.prototype['delete']=function(k){ k=String(k); for(var i=this._p.length-1;i>=0;i--) if(this._p[i][0]===k) this._p.splice(i,1); };\n"
"SP.prototype.forEach=function(f,t){ for(var i=0;i<this._p.length;i++) f.call(t,this._p[i][1],this._p[i][0],this); };\n"
"SP.prototype.keys=function(){ var r=[]; for(var i=0;i<this._p.length;i++) r.push(this._p[i][0]); return r; };\n"
"SP.prototype.toString=function(){ var r=[]; for(var i=0;i<this._p.length;i++) r.push(enc(this._p[i][0])+'='+enc(this._p[i][1])); return r.join('&'); };\n"
"function URL(url,base){\n"
"  if(!(this instanceof URL)) return new URL(url,base);\n"
"  var abs=__url_resolve(String(url), base==null?undefined:String(base));\n"
"  var m=RE.exec(abs)||[];\n"
"  this.href=abs;\n"
"  this.protocol=m[1]||'';\n"
"  var auth=m[2]||'';\n"
"  this.host=auth; this.hostname=auth; this.port='';\n"
"  var at=auth.lastIndexOf('@'); if(at>=0){ auth=auth.slice(at+1); this.host=auth; this.hostname=auth; }\n"
"  var c=auth.lastIndexOf(':');\n"
"  if(c>=0 && auth.indexOf(']')<c){ this.hostname=auth.slice(0,c); this.port=auth.slice(c+1); }\n"
"  this.pathname=m[3]||'';\n"
"  this.search=m[4]||'';\n"
"  this.hash=m[5]||'';\n"
"  this.origin=this.protocol&&this.host?(this.protocol+'//'+this.host):'';\n"
"  this.searchParams=new SP(this.search);\n"
"}\n"
"URL.prototype.toString=function(){ return this.href; };\n"
"if(typeof globalThis.URL==='undefined') globalThis.URL=URL;\n"
/* getElementById is the most-used call on the web and this DOM did not have
 * it -- every page that reached for one got a TypeError and stopped there.
 * It is querySelector with the id escaped, so it costs nothing and cannot
 * drift from the selector engine's own idea of what an id is. */
/* documentElement / body / head / title. A page that cannot reach its own
 * root cannot set a class on it, which is how half the web turns on its own
 * JavaScript-enabled styling. */
"if(document){\n"
"  if(!document.documentElement) document.documentElement=document.querySelector('html');\n"
"  if(!document.body) document.body=document.querySelector('body');\n"
"  if(!document.head) document.head=document.querySelector('head');\n"
"  if(document.title===undefined){ var __t=document.querySelector('title');\n"
"    document.title=__t?__t.textContent:''; }\n"
"}\n"
/* el.style.display = 'none' is the single most common thing a script does to
 * an element, and `style` did not exist -- so it threw on the property before
 * it ever got to the assignment. A Proxy turns each set into the setStyle the
 * DOM already has, with the JS spelling (backgroundColor) folded back to the
 * CSS one (background-color). Declarations accumulate, because a script that
 * sets display and then color means both. */
/* classList over the classAdd/classRemove/classToggle/classContains this DOM
 * already had under its own names. `el.classList.add('x')` is how a page turns
 * anything on; `classAdd` is how nobody does. */
"(function(){ var pr=null;\n"
"  try{ pr=Object.getPrototypeOf(document.createElement('div')); }catch(_){}\n"
"  if(!pr || ('classList' in pr)) return;\n"
"  Object.defineProperty(pr,'classList',{get:function(){ var el=this;\n"
"    function list(){ var c=el.className||''; c=c.trim(); return c?c.split(/\\s+/):[]; }\n"
"    var o={ add:function(){ for(var i=0;i<arguments.length;i++) el.classAdd(String(arguments[i])); },\n"
"            remove:function(){ for(var i=0;i<arguments.length;i++) el.classRemove(String(arguments[i])); },\n"
"            toggle:function(n,f){ if(f===undefined) return el.classToggle(String(n));\n"
"                                  if(f) el.classAdd(String(n)); else el.classRemove(String(n)); return !!f; },\n"
"            contains:function(n){ return !!el.classContains(String(n)); },\n"
"            item:function(i){ return list()[i]||null; },\n"
"            forEach:function(f,t){ list().forEach(f,t); },\n"
"            toString:function(){ return el.className||''; } };\n"
"    Object.defineProperty(o,'length',{get:function(){ return list().length; }});\n"
"    return o; }, configurable:true});\n"
"})();\n"
"(function(){ var pr=null;\n"
"  try{ pr=Object.getPrototypeOf(document.createElement('div')); }catch(_){}\n"
"  if(!pr || ('style' in pr) || typeof Proxy==='undefined') return;\n"
"  function dash(k){ return String(k).replace(/[A-Z]/g,function(m){return '-'+m.toLowerCase();}); }\n"
"  Object.defineProperty(pr,'style',{get:function(){ var el=this;\n"
"    if(!el.__decl) el.__decl={};\n"
"    return new Proxy(el.__decl,{\n"
"      get:function(t,k){ if(k==='setProperty') return function(n,v){ t[dash(n)]=v; flush(el,t); };\n"
"                         if(k==='removeProperty') return function(n){ delete t[dash(n)]; flush(el,t); };\n"
"                         if(k==='cssText') return text(t);\n"
"                         return t[dash(k)]; },\n"
"      set:function(t,k,v){ if(k==='cssText'){ el.setStyle(String(v)); return true; }\n"
"                           t[dash(k)]=String(v); flush(el,t); return true; },\n"
"      deleteProperty:function(t,k){ delete t[dash(k)]; flush(el,t); return true; }\n"
"    }); }, configurable:true});\n"
"  function text(t){ var r=[]; for(var k in t) if(t[k]!=='') r.push(k+':'+t[k]); return r.join(';'); }\n"
"  function flush(el,t){ el.setStyle(text(t)); }\n"
"})();\n"
/* addEventListener at the TOP LEVEL. Pages call it bare as often as they call
 * it on window, and it threw a ReferenceError -- which killed the script on
 * its first line, before anything it went on to do. Forwarded to the document
 * element so a real listener is registered rather than swallowed. */
"if(typeof globalThis.addEventListener!=='function'){\n"
"  globalThis.addEventListener=function(t,f,o){\n"
"    var el=document&&document.documentElement;\n"
"    if(el&&el.addEventListener) el.addEventListener(t,f,o); };\n"
"  globalThis.removeEventListener=function(){};\n"
"  globalThis.dispatchEvent=function(){ return true; };\n"
"}\n"
"if(document && !document.getElementById) document.getElementById=function(id){\n"
"  id=String(id); if(!/^[A-Za-z_][-A-Za-z0-9_]*$/.test(id)) return null;\n"
"  return document.querySelector('#'+id)||null; };\n"
"if(document && !document.getElementsByTagName) document.getElementsByTagName=function(t){\n"
"  return document.querySelectorAll(String(t)); };\n"
"if(document && !document.getElementsByClassName) document.getElementsByClassName=function(c){\n"
"  c=String(c).trim().split(/\\s+/).map(function(x){return '.'+x;}).join('');\n"
"  return c?document.querySelectorAll(c):[]; };\n"
"if(typeof globalThis.URLSearchParams==='undefined') globalThis.URLSearchParams=SP;\n"
"})();\n";

static int eval_one(const char *src, size_t len, const char *name) {
    JSValue v = JS_Eval(g_ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        /* ...WITH THE STACK. "TypeError: not a function" names neither the
         * function nor the line, which is the one thing needed to fix it; the
         * stack turns a shrug into an address. */
        JSValue st = JS_GetPropertyStr(g_ctx, e, "stack");
        const char *sm = JS_IsUndefined(st) ? 0 : JS_ToCString(g_ctx, st);
        if (g_console && m) {
            char b[512];
            snprintf(b, sizeof b, "%s%s%.200s", m, sm ? " | " : "", sm ? sm : "");
            for (char *q = b; *q; q++) if (*q == '\n') *q = ' ';
            g_console(b);
        }
        if (sm) JS_FreeCString(g_ctx, sm);
        JS_FreeValue(g_ctx, st);
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        JS_FreeValue(g_ctx, v);
        return -1;
    }
    JS_FreeValue(g_ctx, v);
    return 0;
}

static void jsdom_prelude(void) {
    eval_one(JSDOM_PRELUDE, sizeof JSDOM_PRELUDE - 1, "<prelude>");
}

int jsdom_eval(const char *src, const char *name) {
    if (!g_ctx || !src) return -1;
    return eval_one(src, strlen(src), name ? name : "<eval>");
}

int jsdom_run_scripts(void) {
    if (!g_ctx || !g_doc) return 0;
    int failed = 0;
    for (int i = 0; i < g_doc->n_js; i++) {
        char name[24];
        snprintf(name, sizeof name, "<script %d>", i + 1);
        /* One script throwing must not stop the next: a page's scripts are
         * independent, and in a browser a broken third-party tag does not
         * blank the document. */
        /* `document.currentScript` IS the element being run, and a page uses it
         * to find where it was written -- SvelteKit's whole bootstrap is
         * `document.currentScript.parentElement`. It is only meaningful DURING
         * a script, so it is set around each one and cleared afterwards, which
         * is also what the DOM specifies. */
        {
            JSValue g = JS_GetGlobalObject(g_ctx);
            JSValue d = JS_GetPropertyStr(g_ctx, g, "document");
            int sn = g_doc->js_node[i];
            JS_SetPropertyStr(g_ctx, d, "currentScript",
                              (sn >= 0 && sn < g_doc->n) ? make_elem(g_ctx, sn) : JS_NULL);
            JS_FreeValue(g_ctx, d);
            JS_FreeValue(g_ctx, g);
        }
        if (eval_one(g_doc->js[i], g_doc->js_len[i], name) != 0) failed++;
        {
            JSValue g = JS_GetGlobalObject(g_ctx);
            JSValue d = JS_GetPropertyStr(g_ctx, g, "document");
            JS_SetPropertyStr(g_ctx, d, "currentScript", JS_NULL);
            JS_FreeValue(g_ctx, d);
            JS_FreeValue(g_ctx, g);
        }
    }
    return failed;
}

/* ---- events ------------------------------------------------------------- */

/* A node is clickable if IT or any ANCESTOR is listening: an event bubbles, so
 * a handler on a <ul> is what makes every <li> inside it a hit target. Getting
 * this wrong the other way -- asking only about the node itself -- is why a
 * delegated listener, which is how most pages are written, appeared dead. */
int jsdom_has_listener(int node) {
    if (!g_doc) return 0;
    for (int a = node; a >= 0; a = g_doc->nodes[a].parent)
        for (int k = 0; k < MAX_LISTENERS; k++)
            if (g_listen[k].used && g_listen[k].node == a && g_listen[k].type == EV_CLICK)
                return 1;
    return 0;
}

/* Fire `type` on `node`, then on each ancestor in turn -- the BUBBLE phase.
 * (There is no capture phase: addEventListener's third argument is ignored,
 * and almost nothing uses it.) `event.target` stays the node the event
 * happened on however far it bubbles, which is the property delegated
 * handlers are written against. stopPropagation ends the walk. */
static int g_prevented;      /* did a handler veto the default action? */

static int dispatch_event(int node, int type) {
    if (!g_ctx || !g_doc) return 0;
    int ran = 0;
    g_prevented = 0;
    for (int a = node; a >= 0; a = g_doc->nodes[a].parent) {
        int stopped = 0;
        for (int k = 0; k < MAX_LISTENERS; k++) {
            if (!g_listen[k].used || g_listen[k].node != a || g_listen[k].type != type) continue;
            JSValue ev = JS_NewObject(g_ctx);
            JS_SetPropertyStr(g_ctx, ev, "type", JS_NewString(g_ctx, EV_NAME[type]));
            JS_SetPropertyStr(g_ctx, ev, "target", make_elem(g_ctx, node));
            JS_SetPropertyStr(g_ctx, ev, "currentTarget", make_elem(g_ctx, a));
            JS_SetPropertyStr(g_ctx, ev, "__stop", JS_NewInt32(g_ctx, 0));
            JS_SetPropertyStr(g_ctx, ev, "stopPropagation",
                JS_NewCFunction(g_ctx, js_stop_propagation, "stopPropagation", 0));
            JS_SetPropertyStr(g_ctx, ev, "__prevented", JS_NewInt32(g_ctx, 0));
            JS_SetPropertyStr(g_ctx, ev, "preventDefault",
                JS_NewCFunction(g_ctx, js_prevent_default, "preventDefault", 0));
            JSValue argv[1] = { ev };
            JSValue r = JS_Call(g_ctx, g_listen[k].fn, JS_UNDEFINED, 1, argv);
            if (JS_IsException(r)) {
                JSValue e = JS_GetException(g_ctx);
                const char *m = JS_ToCString(g_ctx, e);
                if (g_console && m) g_console(m);
                if (m) JS_FreeCString(g_ctx, m);
                JS_FreeValue(g_ctx, e);
            }
            JS_FreeValue(g_ctx, r);
            JSValue st = JS_GetPropertyStr(g_ctx, ev, "__stop");
            int sv = 0; JS_ToInt32(g_ctx, &sv, st); JS_FreeValue(g_ctx, st);
            if (sv) stopped = 1;
            JSValue pd = JS_GetPropertyStr(g_ctx, ev, "__prevented");
            int pv = 0; JS_ToInt32(g_ctx, &pv, pd); JS_FreeValue(g_ctx, pd);
            if (pv) g_prevented = 1;
            JS_FreeValue(g_ctx, ev);
            ran = 1;
        }
        if (stopped) break;
    }
    return ran;
}

int jsdom_dispatch_click(int node)  { return dispatch_event(node, EV_CLICK); }
int jsdom_dispatch_submit(int node) {
    dispatch_event(node, EV_SUBMIT);
    return g_prevented;          /* 1 = a handler said do not navigate */
}
int jsdom_dispatch_input(int node)  {
    int a = dispatch_event(node, EV_INPUT);
    int b = dispatch_event(node, EV_CHANGE);
    return a || b;
}

static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int repeat) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    if (ms < 0) ms = 0;
    /* A floor on repeats. setInterval(f, 0) is a page asking to be run as fast
     * as the machine can go, which on a shared UI thread means the window
     * stops responding -- and the page cannot tell that it did anything wrong.
     * Clamping is kinder than freezing. */
    if (repeat && ms < 10) ms = 10;

    for (int k = 0; k < MAX_TIMERS; k++) {
        if (g_timer[k].used) continue;
        g_timer[k].used = 1;
        g_timer[k].repeat = repeat;
        g_timer[k].every = (unsigned long long)ms;
        g_timer[k].due = g_now + (unsigned long long)ms;
        g_timer[k].fn = JS_DupValue(ctx, argv[0]);
        g_timer[k].id = g_timer_seq++;
        return JS_NewInt32(ctx, g_timer[k].id);
    }
    return JS_ThrowInternalError(ctx, "too many timers");
}
static JSValue js_set_timeout(JSContext *c, JSValueConst t, int n, JSValueConst *a)
{ return js_set_timer(c, t, n, a, 0); }
static JSValue js_set_interval(JSContext *c, JSValueConst t, int n, JSValueConst *a)
{ return js_set_timer(c, t, n, a, 1); }

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    for (int k = 0; k < MAX_TIMERS; k++)
        if (g_timer[k].used && g_timer[k].id == id) {
            JS_FreeValue(ctx, g_timer[k].fn);
            g_timer[k].used = 0;
        }
    return JS_UNDEFINED;
}

/* fetch(url) -> Promise. The Promise is real: QuickJS hands back its resolve
 * and reject functions, we stash them on a slot, and the pump settles the
 * Promise when the bytes land -- which is what makes `await fetch(...)` and
 * `.then(...)` behave as a script author expects rather than as a stub. */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);   /* [resolve, reject] */
    if (JS_IsException(promise)) return promise;

    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : 0;
    int slot = -1;
    for (int k = 0; k < FETCH_MAX; k++) if (!g_fetch[k].used) { slot = k; break; }

    if (!url || slot < 0) {
        /* Reject NOW, on this stack. The pump would work too, but a fetch that
         * cannot even be queued should fail on the same turn it was asked, the
         * way a browser rejects a malformed URL immediately. */
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message",
                          JS_NewString(ctx, url ? "too many concurrent fetches" : "fetch: no URL"));
        JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst[]){ err });
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
        if (url) JS_FreeCString(ctx, url);
        return promise;
    }

    g_fetch[slot].used = 1;
    g_fetch[slot].started = 0;
    snprintf(g_fetch[slot].url, sizeof g_fetch[slot].url, "%s", url);
    g_fetch[slot].resolve = funcs[0];
    g_fetch[slot].reject  = funcs[1];
    JS_FreeCString(ctx, url);
    return promise;
}

/* Settle a fetch slot: build a small Response-shaped object ({ ok, status,
 * text() }) and resolve, or reject with the network error. Deliberately not
 * the whole Fetch spec -- headers, streaming, a real Body -- because a
 * documentation page's script wants status and text, and a binding that
 * pretends to more than it has is the lie this browser refuses. */
static void fetch_settle(int slot, const struct vnet_result *res) {
    JSContext *ctx = g_ctx;
    JSValue rf = g_fetch[slot].resolve, jf = g_fetch[slot].reject;
    g_fetch[slot].used = 0;

    if (res->err[0] && res->len == 0) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, res->err));
        JSValue r = JS_Call(ctx, jf, JS_UNDEFINED, 1, (JSValueConst[]){ err });
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, err);
    } else {
        JSValue body = JS_NewStringLen(ctx, g_fetch_buf, res->len);
        JSValue resp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, res->status / 100 == 2));
        JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, res->status));
        /* text() returns a resolved Promise of the body -- Response.text() is
         * async in the real API, and matching that means `await resp.text()`
         * is what a script writes here too. */
        JSValue textfn = JS_Eval(ctx, "(function(b){return function(){return Promise.resolve(b);};})",
                                 60, "<fetch>", JS_EVAL_TYPE_GLOBAL);
        JSValue bound = JS_Call(ctx, textfn, JS_UNDEFINED, 1, (JSValueConst[]){ body });
        JS_SetPropertyStr(ctx, resp, "text", bound);
        JS_FreeValue(ctx, textfn);
        JSValue r = JS_Call(ctx, rf, JS_UNDEFINED, 1, (JSValueConst[]){ resp });
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, resp); JS_FreeValue(ctx, body);
    }
    JS_FreeValue(ctx, rf);
    JS_FreeValue(ctx, jf);
}

/* Move fetches along: reap the active one, start the next. One at a time on
 * the shared worker, behind the document and its images. */
static int fetch_pump(void) {
    int changed = 0;
    if (g_fetch_active >= 0) {
        struct vnet_result res;
        int r = fetchjob_poll(FETCH_TAG, &res);
        if (r == 1) {
            fetch_settle(g_fetch_active, &res);
            g_fetch_active = -1; changed = 1;
        } else if (r < 0) {                 /* the job vanished (page changed) */
            if (g_fetch_active < FETCH_MAX && g_fetch[g_fetch_active].used) {
                g_fetch[g_fetch_active].used = 0;
                JS_FreeValue(g_ctx, g_fetch[g_fetch_active].resolve);
                JS_FreeValue(g_ctx, g_fetch[g_fetch_active].reject);
            }
            g_fetch_active = -1; changed = 1;
        }
    }
    if (g_fetch_active < 0 && !fetchjob_busy()) {
        for (int k = 0; k < FETCH_MAX; k++) {
            if (!g_fetch[k].used || g_fetch[k].started) continue;
            if (fetchjob_start(g_fetch[k].url, g_fetch_buf, sizeof g_fetch_buf, FETCH_TAG) == 0) {
                g_fetch[k].started = 1;
                g_fetch_active = k;
                changed = 1;
            }
            break;
        }
    }
    return changed;
}

/* Drain QuickJS's pending-job queue: promise reactions, queueMicrotask, and
 * the async continuations `await` compiles to. This is the beating heart of
 * async in the engine, and forgetting it makes every promise a silent no-op. */
static int drain_jobs(void) {
    int did = 0;
    JSContext *c;
    for (;;) {
        int r = JS_ExecutePendingJob(g_rt, &c);
        if (r <= 0) {                        /* 0 = none left, <0 = a job threw */
            if (r < 0 && c) {
                JSValue e = JS_GetException(c);
                const char *m = JS_ToCString(c, e);
                if (g_console && m) g_console(m);
                if (m) JS_FreeCString(c, m);
                JS_FreeValue(c, e);
                did = 1;
                continue;                    /* keep draining past a rejection */
            }
            break;
        }
        did = 1;
    }
    return did;
}

int jsdom_pump(unsigned long long now_ms) {
    if (!g_ctx) return 0;
    g_now = now_ms;
    int changed = 0;
    if (fetch_pump()) changed = 1;

    /* timers */
    for (int k = 0; k < MAX_TIMERS; k++) {
        if (!g_timer[k].used || now_ms < g_timer[k].due) continue;
        JSValue fn = g_timer[k].fn;
        if (g_timer[k].repeat) g_timer[k].due = now_ms + g_timer[k].every;
        else                   g_timer[k].used = 0;
        JSValue r = JS_Call(g_ctx, fn, JS_UNDEFINED, 0, 0);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(g_ctx);
            const char *m = JS_ToCString(g_ctx, e);
            if (g_console && m) g_console(m);
            if (m) JS_FreeCString(g_ctx, m);
            JS_FreeValue(g_ctx, e);
        }
        JS_FreeValue(g_ctx, r);
        if (!g_timer[k].repeat) JS_FreeValue(g_ctx, fn);
        changed = 1;
    }

    /* microtasks LAST: a timer or a settled fetch may have queued promise
     * reactions, and they must run this frame, not next. */
    if (drain_jobs()) changed = 1;
    return changed;
}

unsigned long long jsdom_next_timer(void) {
    unsigned long long best = 0;
    for (int k = 0; k < MAX_TIMERS; k++) {
        if (!g_timer[k].used) continue;
        if (!best || g_timer[k].due < best) best = g_timer[k].due;
    }
    return best;
}

int jsdom_busy(void) {
    for (int k = 0; k < FETCH_MAX; k++) if (g_fetch[k].used) return 1;
    return 0;
}
