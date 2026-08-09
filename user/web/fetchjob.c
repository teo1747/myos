/* user/web/fetchjob.c -- see fetchjob.h.
 *
 * Small on purpose. The concurrency in a browser should be one flag and one
 * thread, not a framework: everything hard about the fetch already lives in
 * net.c, and everything hard about drawing already lives in the toolkit.
 */
#include <string.h>
#include <stdio.h>

#include "embk.h"
#include "fetchjob.h"

enum { JOB_IDLE = 0, JOB_RUNNING, JOB_DONE };

/* `volatile` because the worker writes what the UI thread reads. Both are
 * plain aligned words on x86-64 and stores are not reordered past each other,
 * so `state` published LAST is a sufficient release: by the time the UI sees
 * JOB_DONE, the result and the buffer are complete. */
static volatile int      g_state = JOB_IDLE;
static struct vnet_result g_res;
static char             *g_buf;
static size_t            g_cap;
static char              g_url[512];
static int               g_tid = -1;
static uint64_t          g_started_ms;
static int               g_tag;
static char              g_body[2048];

/* WHERE THE SECONDS GO, per tag. A page that takes two and a half minutes is
 * the loudest complaint the browser has, and "Loading... 146s" says only that
 * it was slow -- not whether that is one big transfer, a hundred small ones,
 * or the parser. Totals are cheap to keep and turn the next perf question from
 * an argument into a reading. */
#define FJ_TAGS 8
static unsigned long g_stat_bytes[FJ_TAGS];
static unsigned long g_stat_ms[FJ_TAGS];
static unsigned      g_stat_n[FJ_TAGS];

void fetchjob_stats(int tag, unsigned long *bytes, unsigned long *ms, unsigned *n) {
    if (tag < 0 || tag >= FJ_TAGS) { if (bytes) *bytes = 0; if (ms) *ms = 0; if (n) *n = 0; return; }
    if (bytes) *bytes = g_stat_bytes[tag];
    if (ms)    *ms    = g_stat_ms[tag];
    if (n)     *n     = g_stat_n[tag];
}
void fetchjob_stats_reset(void) {
    memset(g_stat_bytes, 0, sizeof g_stat_bytes);
    memset(g_stat_ms, 0, sizeof g_stat_ms);
    memset(g_stat_n, 0, sizeof g_stat_n);
}
static int               g_have_body;

static void worker(long arg) {
    (void)arg;
    /* Copy nothing, allocate nothing, touch no UI state -- just the fetch. */
    if (g_have_body) vnet_post(g_url, g_body, g_buf, g_cap, &g_res);
    else             vnet_fetch(g_url, g_buf, g_cap, &g_res);
    g_state = JOB_DONE;                  /* published last: see above */
    embk_thread_exit(0);
}

int fetchjob_start_post(const char *url, const char *body,
                        char *buf, size_t cap, int tag) {
    int rc = fetchjob_start(url, buf, cap, tag);
    if (rc != 0) return rc;
    /* set AFTER the start succeeded, and copied: the form's buffer is the
     * app's and may be reused before the worker reads it */
    snprintf(g_body, sizeof g_body, "%s", body ? body : "");
    g_have_body = 1;
    return 0;
}

int fetchjob_start(const char *url, char *buf, size_t cap, int tag) {
    if (g_state == JOB_RUNNING) return -1;

    /* Reap the previous worker before starting another. A thread that has run
     * to completion still holds its slot until someone joins it, and a browser
     * makes one of these per navigation -- unjoined, they accumulate for the
     * life of the process. */
    if (g_tid >= 0) { embk_thread_join(g_tid); g_tid = -1; }

    memset(&g_res, 0, sizeof g_res);
    snprintf(g_url, sizeof g_url, "%s", url);
    g_buf = buf;
    g_cap = cap;
    g_started_ms = embk_uptime_ms();
    g_tag = tag;
    g_have_body = 0;
    g_state = JOB_RUNNING;                /* set BEFORE the thread exists, so a
                                           * poll racing the spawn sees RUNNING
                                           * rather than IDLE */

    int64_t tid = embk_thread_create(worker, 0);
    if (tid < 0) {
        /* No thread? Then do it here. A frozen window beats no page at all,
         * and this is the path a machine under memory pressure takes. */
        vnet_fetch(g_url, g_buf, g_cap, &g_res);
        g_state = JOB_DONE;
        return 0;
    }
    g_tid = (int)tid;
    return 0;
}

int fetchjob_poll(int tag, struct vnet_result *out) {
    if (g_state == JOB_RUNNING) return 0;
    if (g_state != JOB_DONE)    return -1;
    if (g_tag != tag) return 0;          /* someone else's -- leave it for them */
    if (g_tid >= 0) { embk_thread_join(g_tid); g_tid = -1; }
    /* Charged to the tag that asked for it, at the moment it is handed over. */
    if (tag >= 0 && tag < FJ_TAGS) {
        uint64_t now = embk_uptime_ms();
        g_stat_bytes[tag] += g_res.len;
        g_stat_ms[tag]    += (unsigned long)(now > g_started_ms ? now - g_started_ms : 0);
        g_stat_n[tag]++;
    }
    if (out) *out = g_res;
    g_state = JOB_IDLE;
    g_buf = 0; g_cap = 0;
    return 1;
}

int fetchjob_busy(void) { return g_state == JOB_RUNNING; }

unsigned fetchjob_elapsed_ms(void) {
    if (g_state != JOB_RUNNING) return 0;
    uint64_t now = embk_uptime_ms();
    return (unsigned)(now > g_started_ms ? now - g_started_ms : 0);
}

const char *fetchjob_url(void) { return g_state == JOB_RUNNING ? g_url : ""; }
