/* ports/netsurf/frontend/fetch.c -- what type is this file, and where are the
 * browser's own resources?
 *
 * Not the network. NetSurf's HTTP fetching is a separate thing entirely
 * (content/fetchers/, and this port builds with NETSURF_USE_CURL=NO so that
 * ours can go in over the TLS stack the OS already has). What the FETCH TABLE
 * answers is narrower and both parts are needed before a page can load at all:
 *
 *   filetype      -- the MIME type of a local file, by extension, because
 *                    there is no /etc/mime.types here and nothing to read it
 *   resource url  -- where the browser's OWN files live: its default
 *                    stylesheet, its error pages, its favicon. Without these
 *                    a page renders with no user-agent CSS at all, which is
 *                    every element as an inline box.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/fetch.h"
#include "utils/nsurl.h"
#include "emblink.h"


/* Where the resources were installed on the image. Fixed, because the OS's
 * namespace is per-process and /system is read-only by construction (see
 * docs/USERSPACE_v2.md) -- there is nowhere else they could be. */
#ifndef EMBLINK_RESPATH
#define EMBLINK_RESPATH "/system/netsurf"
#endif

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL ? dot + 1 : "";
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* By extension, and the default is text/html rather than
 * application/octet-stream: a local file with no extension is nearly always a
 * document here, and offering to download it instead of showing it is the
 * more annoying way to be wrong. */
static const char *fetch_filetype(const char *unix_path)
{
    static const struct { const char *ext, *mime; } map[] = {
        { "html", "text/html" },  { "htm",  "text/html" },
        { "css",  "text/css" },   { "txt",  "text/plain" },
        { "js",   "application/javascript" },
        { "json", "application/json" },
        { "xml",  "text/xml" },   { "svg",  "image/svg+xml" },
        { "png",  "image/png" },  { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" }, { "gif",  "image/gif" },
        { "bmp",  "image/bmp" },  { "ico",  "image/x-icon" },
        { "webp", "image/webp" },
    };
    const char *ext = ext_of(unix_path);
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (ieq(ext, map[i].ext)) return map[i].mime;
    return "text/html";
}

/* resource: URLs name the browser's own files. The core asks for a handful by
 * name at startup (adblock.css, default.css, quirks.css, favicon.ico...) and
 * maps each to a real path here. */
static struct nsurl *fetch_get_resource_url(const char *path)
{
    char buf[512];
    struct nsurl *url = NULL;
    snprintf(buf, sizeof buf, "file://" EMBLINK_RESPATH "/%s", path);
    if (nsurl_create(buf, &url) != NSERROR_OK) return NULL;
    return url;
}

static struct gui_fetch_table fetch_table = {
    .filetype = fetch_filetype,
    .get_resource_url = fetch_get_resource_url,
};

struct gui_fetch_table *emblink_fetch_table = &fetch_table;
