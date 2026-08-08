/* user/web/tabs.c -- see tabs.h. */
#include <string.h>
#include <stdio.h>

#include "tabs.h"

struct tab {
    int    used;
    char   url[TAB_URL_MAX];
    char   title[TAB_TITLE_MAX];
    char   label[TAB_TITLE_MAX];
    float  scroll;
    float  zoom;
    size_t src_len;
    /* the back / forward stacks, per tab -- see tabs.h */
    char   back[TAB_HIST_MAX][TAB_URL_MAX]; int back_n;
    char   fwd[TAB_HIST_MAX][TAB_URL_MAX];  int fwd_n;
};

static struct tab g_tab[TAB_MAX];
/* Kept OUT of struct tab so the array of tabs stays something you can look at
 * in a debugger without paging through half a megabyte per element. */
static char g_src[TAB_MAX][TAB_SRC_MAX];
static int  g_cur;
static int  g_ready;

static int valid(int i) { return i >= 0 && i < TAB_MAX && g_tab[i].used; }

void tab_init(void) {
    if (g_ready) return;
    g_ready = 1;
    memset(g_tab, 0, sizeof g_tab);
    g_tab[0].used = 1;
    g_tab[0].zoom = 1.0f;
    g_cur = 0;
}

int tab_count(void) {
    int n = 0;
    for (int i = 0; i < TAB_MAX; i++) if (g_tab[i].used) n++;
    return n;
}

int tab_is_open(int i) { return valid(i); }

int tab_current(void) { return g_cur; }

int tab_open(const char *url) {
    tab_init();
    for (int i = 0; i < TAB_MAX; i++) {
        if (g_tab[i].used) continue;
        memset(&g_tab[i], 0, sizeof g_tab[i]);
        g_tab[i].used = 1;
        g_tab[i].zoom = 1.0f;
        snprintf(g_tab[i].url, sizeof g_tab[i].url, "%s", url ? url : "");
        return i;
    }
    return -1;
}

int tab_close(int i) {
    if (!valid(i) || tab_count() <= 1) return g_cur;
    g_tab[i].used = 0;
    if (g_cur == i) {
        /* Land on the NEIGHBOUR, preferring the one to the left. Jumping to
         * tab 0 from the middle of a row loses your place in a way that reads
         * as the browser having done something else entirely. */
        int n = -1;
        for (int k = i - 1; k >= 0; k--) if (g_tab[k].used) { n = k; break; }
        if (n < 0) for (int k = i + 1; k < TAB_MAX; k++) if (g_tab[k].used) { n = k; break; }
        g_cur = n < 0 ? 0 : n;
    }
    return g_cur;
}

int tab_select(int i) {
    if (!valid(i) || i == g_cur) return -1;
    g_cur = i;
    return 0;
}

const char *tab_url(int i)   { return valid(i) ? g_tab[i].url   : ""; }
const char *tab_title(int i) { return valid(i) ? g_tab[i].title : ""; }

void tab_set_url(int i, const char *u) {
    if (valid(i)) snprintf(g_tab[i].url, sizeof g_tab[i].url, "%s", u ? u : "");
}
void tab_set_title(int i, const char *t) {
    if (valid(i)) snprintf(g_tab[i].title, sizeof g_tab[i].title, "%s", t ? t : "");
}

/* The name on the tab. A title if the page gave one; otherwise the last
 * meaningful piece of the URL, which is what a person recognises -- "index" is
 * useless, so a trailing slash falls back to the host. Never empty. */
const char *tab_label(int i) {
    if (!valid(i)) return "";
    struct tab *t = &g_tab[i];
    if (t->title[0]) {
        /* memmove, not snprintf: both live in the same struct, and passing
         * overlapping buffers to a restrict-qualified argument is undefined
         * even when it happens to work. */
        size_t n = strlen(t->title);
        if (n >= sizeof t->label) n = sizeof t->label - 1;
        memmove(t->label, t->title, n);
        t->label[n] = 0;
        return t->label;
    }
    const char *u = t->url;
    if (!u[0]) return "New tab";

    /* strip the scheme, then take the host or the last path segment */
    const char *p = strstr(u, "://");
    const char *host = p ? p + 3 : u;
    const char *last = host;
    for (const char *q = host; *q; q++) if (*q == '/' && q[1]) last = q + 1;
    /* a trailing slash left `last` at the host, which is the right answer */
    size_t n = 0;
    while (last[n] && last[n] != '?' && last[n] != '#') n++;
    /* "c.example/" is the host with punctuation on the end, and the slash is
     * the reason `last` stayed there. Drop it, or every site's front page is
     * labelled with a stray character. */
    while (n > 1 && last[n - 1] == '/') n--;
    if (n == 0) {
        n = strlen(u);
        if (n >= sizeof t->label) n = sizeof t->label - 1;
        memmove(t->label, u, n);
        t->label[n] = 0;
        return t->label;
    }
    if (n >= sizeof t->label) n = sizeof t->label - 1;
    memcpy(t->label, last, n);
    t->label[n] = 0;
    return t->label;
}

float tab_scroll(int i) { return valid(i) ? g_tab[i].scroll : 0.0f; }
void  tab_set_scroll(int i, float y) { if (valid(i)) g_tab[i].scroll = y; }

float tab_zoom(int i) {
    if (!valid(i)) return 1.0f;
    /* A zero here means a tab that predates the field or was memset -- read as
     * "unzoomed" rather than as "invisible". */
    return g_tab[i].zoom > 0.0f ? g_tab[i].zoom : 1.0f;
}
void  tab_set_zoom(int i, float z) { if (valid(i) && z > 0.0f) g_tab[i].zoom = z; }

char  *tab_src(int i)     { return valid(i) ? g_src[i] : 0; }
size_t tab_src_len(int i) { return valid(i) ? g_tab[i].src_len : 0; }

void tab_set_src_len(int i, size_t n) {
    if (!valid(i)) return;
    g_tab[i].src_len = n > TAB_SRC_MAX ? TAB_SRC_MAX : n;
}

size_t tab_set_src(int i, const char *bytes, size_t n) {
    if (!valid(i) || !bytes) return 0;
    if (n > TAB_SRC_MAX) n = TAB_SRC_MAX;
    memcpy(g_src[i], bytes, n);
    g_tab[i].src_len = n;
    return n;
}

/* --- back / forward ------------------------------------------------------ *
 *
 * The same shape the single-page browser had, one copy per tab. Pushing a new
 * place CLEARS forward, because you have branched: keeping it would offer to
 * go "forward" to a page that is no longer on any path from here.
 */
void tab_hist_push(int i, const char *url) {
    if (!valid(i) || !url || !url[0]) return;
    struct tab *t = &g_tab[i];
    if (t->back_n == TAB_HIST_MAX) {
        memmove(t->back[0], t->back[1], sizeof t->back[0] * (TAB_HIST_MAX - 1));
        t->back_n--;
    }
    snprintf(t->back[t->back_n++], TAB_URL_MAX, "%s", url);
    t->fwd_n = 0;
}

int tab_can_back(int i) { return valid(i) && g_tab[i].back_n > 0; }
int tab_can_fwd(int i)  { return valid(i) && g_tab[i].fwd_n  > 0; }

int tab_hist_back(int i, const char *current, char *out, size_t cap) {
    if (!tab_can_back(i)) return -1;
    struct tab *t = &g_tab[i];
    if (current && current[0] && t->fwd_n < TAB_HIST_MAX)
        snprintf(t->fwd[t->fwd_n++], TAB_URL_MAX, "%s", current);
    snprintf(out, cap, "%s", t->back[--t->back_n]);
    return 0;
}

int tab_hist_fwd(int i, const char *current, char *out, size_t cap) {
    if (!tab_can_fwd(i)) return -1;
    struct tab *t = &g_tab[i];
    if (current && current[0] && t->back_n < TAB_HIST_MAX)
        snprintf(t->back[t->back_n++], TAB_URL_MAX, "%s", current);
    snprintf(out, cap, "%s", t->fwd[--t->fwd_n]);
    return 0;
}
