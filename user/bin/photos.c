/* user/bin/photos.c -- the picture viewer.
 *
 * Ours, not a port. The decoders were already ours (png.c and jpeg.c, written
 * for the browser), the toolkit is ours, and what was missing was the thing in
 * between: the part that decides what to show, at what size, and does the
 * scaling well enough that a photograph looks like a photograph.
 *
 * THE VIEWPORT IS THE UNIT OF WORK. The obvious design scales the whole
 * picture and pans around the result; this one computes which SOURCE rectangle
 * is visible and resamples exactly that, straight to the size of the window.
 * At 8x zoom the obvious design does fifty times the work for the same pixels,
 * and on a 50-megapixel photo it also needs a second 200MB buffer to put them
 * in. Here the cost of a frame is the size of the WINDOW, whatever the file
 * is -- so zooming into a huge photo costs what zooming into a small one does.
 *
 * The controls are the ones you already know from every other viewer: Left and
 * Right walk the folder, +/- zoom, 0 fits, 1 is actual size, drag pans, and
 * ESC closes. None of that is interesting, and that is the point -- an app
 * whose bindings have to be learned is an app that got something wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "photo.h"

/* Chrome heights, used to work out how much room the picture actually has.
 * Deliberately a little generous: guessing SMALL means the fit scale comes out
 * a hair under and the picture never quite touches the edge, where guessing
 * large means it overflows into the bar on the first frame of every image. */
#define BAR_H     52.0f
#define STATUS_H  30.0f
#define PAD       10.0f

#define ZOOM_MIN  0.02f
#define ZOOM_MAX  32.0f
#define ZOOM_STEP 1.25f

/* Where to look when nothing was named on the command line. */
#define DEFAULT_DIR "/data/pictures"

static Album g_album;
static Photo g_img;
static int   g_rc = PHOTO_OK;
static char  g_path[320];
static char  g_name[96] = "";

static bool  g_fit   = true;      /* scale to the window                     */
static float g_scale = 1.0f;      /* when not fitting                        */
static float g_cx = 0.5f;         /* view centre, as a fraction of the image */
static float g_cy = 0.5f;
static bool  g_info = true;

/* The resampled tile currently on screen, and what it was built for. */
static uint32_t *g_tile;
static uint32_t  g_tw, g_th;      /* its real pixel size                     */
static float     g_dw, g_dh;      /* the size it is DRAWN at                 */
static uint64_t  g_sig;           /* rebuild when this changes               */
static float     g_cur_scale = 1.0f;

static bool  g_dragging;
static float g_drag_x, g_drag_y;

static char g_title[192], g_status[256], g_zoomlbl[16], g_count[32];

/* --- loading -------------------------------------------------------------- */

static void drop_tile(void)
{
    free(g_tile);
    g_tile = NULL;
    g_sig = 0;
}

static void load_path(const char *p)
{
    photo_free(&g_img);
    drop_tile();
    snprintf(g_path, sizeof g_path, "%s", p);
    const char *slash = strrchr(p, '/');
    snprintf(g_name, sizeof g_name, "%s", slash ? slash + 1 : p);

    g_rc = photo_load(p, &g_img);
    /* Every new picture starts fitted and centred. Carrying the previous
     * picture's zoom across is a real choice some viewers make, and it means
     * stepping through a folder shows you the top-left corner of every third
     * one. */
    g_fit = true;
    g_cx = g_cy = 0.5f;
}

static void load_index(int i)
{
    char buf[320];
    if (album_path(&g_album, i, buf, sizeof buf)) load_path(buf);
}

/* --- the visible tile ----------------------------------------------------- */

static void rebuild(float availw, float availh)
{
    if (g_rc != PHOTO_OK || !g_img.px) return;

    float sw = (float)g_img.w, sh = (float)g_img.h;

    /* FIT NEVER ENLARGES. Blowing a 64x64 icon up to fill a 900px window is
     * technically "fitting" it and is never what anyone wanted to see. */
    float fit = availw / sw;
    if (availh / sh < fit) fit = availh / sh;
    if (fit > 1.0f) fit = 1.0f;

    float s = g_fit ? fit : g_scale;
    if (s < ZOOM_MIN) s = ZOOM_MIN;
    if (s > ZOOM_MAX) s = ZOOM_MAX;
    g_cur_scale = s;

    /* How much of the source fits in the window at this scale. */
    float visw = availw / s, vish = availh / s;
    uint32_t cw = visw >= sw ? g_img.w : (uint32_t)(visw + 0.5f);
    uint32_t ch = vish >= sh ? g_img.h : (uint32_t)(vish + 0.5f);
    if (cw == 0) cw = 1;
    if (ch == 0) ch = 1;

    /* Keep the crop inside the picture. Clamping the CENTRE rather than the
     * origin is what stops a pan from sliding empty space in at the edge. */
    float hx = (float)cw / (2.0f * sw), hy = (float)ch / (2.0f * sh);
    if (g_cx < hx) g_cx = hx;
    if (g_cx > 1.0f - hx) g_cx = 1.0f - hx;
    if (g_cy < hy) g_cy = hy;
    if (g_cy > 1.0f - hy) g_cy = 1.0f - hy;

    float fx = g_cx * sw - (float)cw * 0.5f;
    float fy = g_cy * sh - (float)ch * 0.5f;
    uint32_t x0 = fx <= 0.0f ? 0 : (uint32_t)(fx + 0.5f);
    uint32_t y0 = fy <= 0.0f ? 0 : (uint32_t)(fy + 0.5f);
    if (x0 + cw > g_img.w) x0 = g_img.w - cw;
    if (y0 + ch > g_img.h) y0 = g_img.h - ch;

    uint32_t dw = (uint32_t)((float)cw * s + 0.5f);
    uint32_t dh = (uint32_t)((float)ch * s + 0.5f);
    if (dw == 0) dw = 1;
    if (dh == 0) dh = 1;

    /* Resampling is the expensive thing in this program, so it happens when
     * the ANSWER would differ and not once a frame. Everything the result
     * depends on goes into the signature. */
    uint64_t sig = ((uint64_t)x0 << 44) ^ ((uint64_t)y0 << 24)
                 ^ ((uint64_t)cw << 12) ^ (uint64_t)ch
                 ^ ((uint64_t)dw << 33) ^ ((uint64_t)dh << 3);
    if (sig == g_sig && g_tile) {
        g_dw = (float)dw; g_dh = (float)dh;
        return;
    }

    uint32_t tw = 0, th = 0;
    uint32_t *tile = photo_view(g_img.px, g_img.w, g_img.h,
                                x0, y0, cw, ch, dw, dh, &tw, &th);
    if (!tile) return;                /* keep showing the old one */

    free(g_tile);
    g_tile = tile;
    g_tw = tw; g_th = th;
    g_dw = (float)dw; g_dh = (float)dh;
    g_sig = sig;
}

/* --- input ---------------------------------------------------------------- */

static void zoom_by(float f)
{
    /* Zooming out of FIT starts from where fit left it, so the first press
     * moves one step from what is on screen rather than jumping to 100%. */
    g_scale = (g_fit ? g_cur_scale : g_scale) * f;
    if (g_scale < ZOOM_MIN) g_scale = ZOOM_MIN;
    if (g_scale > ZOOM_MAX) g_scale = ZOOM_MAX;
    g_fit = false;
}

static void pan(float dx_src, float dy_src)
{
    if (!g_img.w || !g_img.h) return;
    g_cx -= dx_src / (float)g_img.w;
    g_cy -= dy_src / (float)g_img.h;
}

static void handle_drag(void)
{
    float px = 0, py = 0;
    ui_pointer_pos(&px, &py);
    bool down = ui_pointer_down();

    if (!down) { g_dragging = false; return; }
    if (!g_dragging) {
        /* Do not start a pan on the title bar -- that gesture already means
         * "move the window", and stealing it would make the window untouchable
         * whenever a picture happened to be zoomed in. */
        if (py <= BAR_H) return;
        g_dragging = true;
        g_drag_x = px; g_drag_y = py;
        return;
    }

    float s = g_cur_scale > 0 ? g_cur_scale : 1.0f;
    pan((px - g_drag_x) / s, (py - g_drag_y) / s);
    g_drag_x = px; g_drag_y = py;
    em_request_frame();
}

static int on_key(int ch)
{
    float step = 64.0f / (g_cur_scale > 0 ? g_cur_scale : 1.0f);

    switch (ch) {
    case EMBK_KEY_LEFT:  load_index(album_step(&g_album, -1)); break;
    case EMBK_KEY_RIGHT: load_index(album_step(&g_album, +1)); break;
    case EMBK_KEY_UP:    pan(0, step);  break;
    case EMBK_KEY_DOWN:  pan(0, -step); break;
    case '+': case '=':  zoom_by(ZOOM_STEP);        break;
    case '-': case '_':  zoom_by(1.0f / ZOOM_STEP); break;
    case '0': g_fit = true; g_cx = g_cy = 0.5f;     break;
    case '1': g_fit = false; g_scale = 1.0f;        break;
    case 'i': case 'I': g_info = !g_info;           break;
    default: return 0;
    }
    em_request_frame();
    return 1;
}

/* --- the view -------------------------------------------------------------- */

static void labels(void)
{
    snprintf(g_title, sizeof g_title, "%s", g_name[0] ? g_name : "Photos");
    snprintf(g_zoomlbl, sizeof g_zoomlbl, "%d%%", (int)(g_cur_scale * 100.0f + 0.5f));

    if (g_album.count > 1)
        snprintf(g_count, sizeof g_count, "%d of %d",
                 g_album.index + 1, g_album.count);
    else
        g_count[0] = '\0';

    if (g_rc != PHOTO_OK) {
        snprintf(g_status, sizeof g_status, "%s", photo_error(g_rc));
        return;
    }
    /* Kilobytes, and the EXIF rotation when one was applied -- a viewer that
     * silently rotates a picture and never says so is one you cannot tell
     * apart from a decoder that got the dimensions wrong. */
    char rot[32] = "";
    if (g_img.orientation != 1)
        snprintf(rot, sizeof rot, "  ·  rotated (EXIF %d)", g_img.orientation);
    snprintf(g_status, sizeof g_status, "%u × %u  ·  %s  ·  %u KB%s",
             g_img.w, g_img.h, g_img.format,
             (unsigned)((g_img.file_bytes + 1023) / 1024), rot);
}

static void PhotosView(void)
{
    float vw = em_viewport_width(), vh = em_viewport_height();
    float availw = vw - PAD * 2.0f;
    float availh = vh - BAR_H - (g_info ? STATUS_H : 0.0f) - PAD * 2.0f;
    if (availw < 32.0f) availw = 32.0f;
    if (availh < 32.0f) availh = 32.0f;

    handle_drag();
    rebuild(availw, availh);
    labels();

    Window("Photos", .corner = 14, .clip = 1) {
        AppBar(g_title) {
            if (Button("−").ghost().clicked()) { zoom_by(1.0f / ZOOM_STEP); }
            Text(g_zoomlbl).caption().secondary();
            if (Button("+").ghost().clicked()) { zoom_by(ZOOM_STEP); }
            if (Button("Fit").ghost().clicked()) { g_fit = true; g_cx = g_cy = 0.5f; }
            if (Button("1:1").ghost().clicked()) { g_fit = false; g_scale = 1.0f; }
        }

        VStack(.grow = 1, .align = Center, .justify = Center,
               .background = T.bg, .clip = 1, .padding = PAD) {
            if (g_rc == PHOTO_OK && g_tile) {
                /* A STABLE key: the toolkit matches instances by it, and the
                 * pixels are handed over fresh every frame anyway. A key that
                 * changed with the tile would discard and rebuild the node on
                 * every pan. */
                ui_image_sized(0x9107, g_tile, g_tw, g_th, g_dw, g_dh);
            } else {
                VStack(.spacing = 6, .align = Center) {
                    Text(g_name[0] ? g_name : "No picture").heading();
                    Text(photo_error(g_rc)).caption().secondary();
                }
            }
        }

        if (g_info) {
            HStack(.px = 14, .height = STATUS_H, .align = Center,
                   .background = T.surface_alt) {
                Text(g_status).caption().secondary();
                Spacer();
                if (g_count[0]) Text(g_count).caption().tertiary();
            }
        }
    }
}

/* --- entry ----------------------------------------------------------------- *
 * Own main rather than EM_APPLICATION, because EM_APPLICATION generates
 * main(void) and this program's whole job is the argument it was given. */
static EmApp g_spec = {
    .title  = "Photos",
    .size   = { 900, 640 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = PhotosView,
};

int main(int argc, char **argv)
{
    if (argc > 1) {
        album_open(&g_album, argv[1]);
        if (g_album.index >= 0) load_index(g_album.index);
        else                    load_path(argv[1]);
    } else {
        /* No argument: open the folder rather than refusing. A viewer started
         * from a launcher has no argument by definition, and "usage:" on a
         * window is not a thing a person can act on. */
        album_open(&g_album, DEFAULT_DIR "/");
        if (g_album.count > 0) { g_album.index = 0; load_index(0); }
        else { g_rc = PHOTO_ENOENT; snprintf(g_name, sizeof g_name, "%s", DEFAULT_DIR); }
    }

    em_set_key_hook(on_key);
    return em_app_run(&g_spec);
}
