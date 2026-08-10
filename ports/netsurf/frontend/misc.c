/* ports/netsurf/frontend/misc.c -- the core's clock.
 *
 * `schedule(t, cb, p)` means "call cb(p) in t milliseconds", and it is how
 * NetSurf drives everything that is not a direct response to input: retrying a
 * fetch, animating a GIF, reflowing after a stylesheet arrives. Passing a
 * negative t CANCELS a pending (cb, p) pair -- the same pair, not just the same
 * callback, which matters because one callback schedules itself per object.
 *
 * Registering the same (cb, p) twice must not queue it twice: the core relies
 * on a re-schedule REPLACING the pending one, and a list that appends instead
 * runs the callback as many times as it was asked to wait.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* newlib hides CLOCK_MONOTONIC behind __POSIX_VISIBLE, and which feature
 * macros survive to this file depends on flags the netsurf build sets for
 * reasons of its own -- turning Duktape on was enough to lose it. The value is
 * newlib's own (time.h, CLOCK_MONOTONIC = 4) and the kernel's clock_gettime
 * switches on exactly that, so naming it here changes nothing except whether
 * this file compiles. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
#endif
#include <string.h>

#include "utils/errors.h"
#include "netsurf/misc.h"
#include "utils/nsurl.h"
#include "emblink.h"


/* Bounded on purpose: an unbounded queue is a page's way of asking for all the
 * memory. Past this a schedule is REFUSED and says so, rather than being
 * dropped -- a silently dropped timer is a browser that stops halfway. */
#define SCHED_MAX 256

struct entry {
    void (*cb)(void *p);
    void *p;
    int64_t due_ms;
    bool live;
};

static struct entry g_sched[SCHED_MAX];
static int g_nsched;

/* Milliseconds since some fixed point. The core only ever compares these. */
static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

nserror emblink_schedule(int tms, void (*cb)(void *p), void *p)
{
    /* find an existing (cb, p) first: re-scheduling REPLACES */
    for (int i = 0; i < g_nsched; i++) {
        if (g_sched[i].live && g_sched[i].cb == cb && g_sched[i].p == p) {
            if (tms < 0) { g_sched[i].live = false; return NSERROR_OK; }
            g_sched[i].due_ms = now_ms() + tms;
            return NSERROR_OK;
        }
    }
    if (tms < 0) return NSERROR_OK;          /* cancelling something not queued */

    for (int i = 0; i < g_nsched; i++) {
        if (!g_sched[i].live) {
            g_sched[i] = (struct entry){ cb, p, now_ms() + tms, true };
            return NSERROR_OK;
        }
    }
    if (g_nsched >= SCHED_MAX) return NSERROR_NOSPACE;
    g_sched[g_nsched++] = (struct entry){ cb, p, now_ms() + tms, true };
    return NSERROR_OK;
}

int emblink_schedule_run(void)
{
    int64_t t = now_ms();
    int64_t soonest = -1;

    /* Snapshot the due set BEFORE running any of it: a callback commonly
     * schedules its own successor, and running straight out of the live list
     * would run that successor in the same pass -- an animation would spin as
     * fast as the loop instead of at its frame interval. */
    for (int i = 0; i < g_nsched; i++) {
        if (!g_sched[i].live) continue;
        if (g_sched[i].due_ms <= t) {
            void (*cb)(void *) = g_sched[i].cb;
            void *p = g_sched[i].p;
            g_sched[i].live = false;         /* before the call: it may re-add */
            cb(p);
        }
    }
    for (int i = 0; i < g_nsched; i++) {
        if (!g_sched[i].live) continue;
        int64_t wait = g_sched[i].due_ms - t;
        if (wait < 0) wait = 0;
        if (soonest < 0 || wait < soonest) soonest = wait;
    }
    return (int)soonest;
}

void emblink_schedule_finalise(void)
{
    memset(g_sched, 0, sizeof g_sched);
    g_nsched = 0;
}

static nserror gui_schedule(int t, void (*callback)(void *p), void *p)
{
    return emblink_schedule(t, callback, p);
}

static void gui_quit(void)
{
}

/* Opening a URL outside the browser. There is nothing outside the browser to
 * open it in yet, so this says so rather than failing silently. */
static nserror gui_launch_url(struct nsurl *url)
{
    (void)url;
    return NSERROR_NOT_IMPLEMENTED;
}

static struct gui_misc_table misc_table = {
    .schedule = gui_schedule,
    .quit = gui_quit,
    .launch_url = gui_launch_url,
};

struct gui_misc_table *emblink_misc_table = &misc_table;
