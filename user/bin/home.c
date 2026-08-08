/* user/bin/home.c -- the EmbLink OS HOME launcher.
 *
 * This is where the OS lands: the kernel spawns it at boot (see kernel/main.c),
 * and it takes over the whole screen as the compositor's DESKTOP layer -- a
 * full-screen, chromeless, back-pinned window (embk_win_create_desktop). App
 * windows the user launches float ON TOP of it.
 *
 * It draws a simple, nice launcher (a title + a grid of app tiles) with the
 * EmUI toolkit, rendering straight into the desktop window's shared pixel
 * pages (zero-copy). Clicks are routed to it by the compositor via
 * embk_win_input (the desktop receives every click that falls through the
 * floating windows), and clicking a tile embk_spawn()s that app. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "embk.h"
#include "appauth.h"
#include "oscfg.h"   /* the user's dock preferences, re-read live */
#include "kit.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"

static uint8_t *read_file(const char *path, size_t *len) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (n + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); }
        int64_t got = embk_read(fd, buf + n, 65536);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    *len = n;
    return buf;
}

/* set by a tile click during the UI pass; the loop acts on it after the frame */
static const char *g_launch = 0;
static const char *g_launch_dir = 0;
static char        g_clock[32] = "up 0:00";
static char        g_datetime[48] = "--:--";
static float       g_sw = 0, g_sh = 0;   /* screen size (the desktop window) */
static char      **g_session_env = 0;

static void launch_folder(const char *path) {
    g_launch = "/data/apps/files/files.elf";
    g_launch_dir = path;
}

/* An app the user can click (launch/open) OR drag. `app` spawns an elf; `dir`
 * opens Files at a folder (one of the two is set). icon/label are BUFFERS so an
 * app can supply them from its own <name>.app presentation manifest; the stable
 * IDENTITY (for reconciler keys) is the app/dir path, never the icon buffer. */
struct app_item {
    char icon[96]; char label[24]; const char *app; const char *dir;
    /* Free placement on the desktop: every icon owns its spot rather than
     * inheriting one from its position in the list. `placed` starts 0 so a
     * newly-arrived icon is auto-arranged instead of landing at (0,0). */
    float x, y; int placed;
};

/* Desktop icon cell: the glyph plus its caption, and the gap around it.
 * DESK_ICON is the one knob for how big desktop apps look. Keep it 8 above a
 * level in the .eic ladder (docs/ICONS.md) -- the art is drawn into size-8, so
 * 48 lands exactly on the 40 level and needs no resampling. */
#define DESK_ICON   48
#define DESK_CELL_W 76
#define DESK_CELL_H 76
#define DESK_MARGIN 16

/* Apps live EITHER on the desktop OR in the dock; dragging MOVES one between the
 * two (never a copy), so a name never lingers behind. Home's folder is $HOME,
 * filled in once at startup. */
static struct app_item g_desk[16] = {
    { "/system/images/setting.eic", "System", 0, "/system" },
};
static int g_desk_n = 1;

/* the bottom app dock -- a centered pill sized to its apps. Grows as apps are
 * dragged in / shrinks as they are dragged out, but never below DOCK_MIN. */
#define DOCK_MIN 2
static struct app_item g_dock[16] = {
    { "/system/images/file.eic",     "Files",    "/data/apps/files/files.elf",       0 },
    { "/system/images/terminal.eic", "Terminal", "/data/apps/term/term.elf",         0 },
    { "/system/images/setting.eic",  "Settings", "/data/apps/settings/settings.elf", 0 },
    { "/system/images/file.eic",     "Vellum",   "/data/apps/vellum/vellum.elf",     0 },
};
static int g_dock_n = 4;

/* Fill an item's icon+label from the app's OWN presentation manifest at
 * /data/apps/<name>/<name>.app ("name X" / "icon Y" lines) -- so the desktop
 * reflects what the app declares, not a hard-coded table. Missing manifest or
 * field => the pre-seeded fallback stays. */
static int load_app_meta(const char *name, char *icon, size_t ic, char *label, size_t lc) {
    char path[128];
    snprintf(path, sizeof path, "/data/apps/%s/%s.app", name, name);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return 0;
    for (size_t i = 0; i < len; ) {
        while (i < len && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i >= len) break;
        if (buf[i] == '#') { while (i < len && buf[i] != '\n') i++; continue; }
        size_t ks = i;
        while (i < len && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        size_t kl = i - ks;
        while (i < len && (buf[i]==' '||buf[i]=='\t')) i++;
        size_t vs = i;
        while (i < len && buf[i]!='\n' && buf[i]!='\r') i++;
        size_t vl = i - vs;
        while (vl && (buf[vs+vl-1]==' '||buf[vs+vl-1]=='\t')) vl--;
        char *dst = 0; size_t cap = 0;
        if (kl==4 && !memcmp(buf+ks,"name",4)) { dst = label; cap = lc; }
        else if (kl==4 && !memcmp(buf+ks,"icon",4)) { dst = icon; cap = ic; }
        if (dst && cap) { size_t n = vl < cap-1 ? vl : cap-1; memcpy(dst, buf+vs, n); dst[n] = 0; }
    }
    free(buf);
    return 1;
}

/* --- the Apps launcher: a grid of ALL installed apps, read from /data/apps ---
 * Each app self-describes via its <name>.app (name + icon); click to launch. */
struct grid_app { char name[32]; char icon[96]; char label[24]; char exec[80]; };
static struct grid_app g_all[48];
static int g_all_n = 0;
static int   g_apps_open = 0;     /* is the launcher grid showing? */
static int   g_apps_frames = 0;   /* debounce the click that opened it */
static float g_apps_scroll = 0;   /* launcher grid scroll offset */
static char  g_apps_q[40];        /* the launcher's search text */

/* Enumerate /data/apps/<name>/, keeping those that hold a <name>.elf, and read
 * each one's presentation manifest. home's namespace grants `ro /data/apps`. */
static void scan_apps(void) {
    g_all_n = 0;
    struct embk_dirent ents[64];
    int64_t n = embk_readdir("/data/apps", ents, 64);
    for (int64_t i = 0; i < n && g_all_n < 48; i++) {
        if (ents[i].type != EMBK_DT_DIR) continue;
        const char *nm = ents[i].name;
        if (nm[0] == '.') continue;
        struct grid_app *a = &g_all[g_all_n];
        snprintf(a->exec, sizeof a->exec, "/data/apps/%s/%s.elf", nm, nm);
        struct embk_stat st;
        if (embk_stat(a->exec, &st) < 0) continue;   /* only real, launchable apps */
        snprintf(a->name, sizeof a->name, "%s", nm);
        snprintf(a->icon, sizeof a->icon, "/system/images/app_launcher.eic");  /* fallback */
        snprintf(a->label, sizeof a->label, "%s", nm);
        /* only apps that DECLARE themselves (ship a <name>.app) appear in the
         * launcher -- internal tools/tests stay out of the user's face. */
        if (!load_app_meta(nm, a->icon, sizeof a->icon, a->label, sizeof a->label)) continue;
        g_all_n++;
    }
}

/* --- drag-and-drop state (a desktop icon INTO the dock, or a dock icon OUT) --- */
static int   g_drag = 0;        /* 0 none, 1 dragging a desktop source, 2 a dock item */
static int   g_drag_i = -1;     /* index in g_desk (kind 1) or g_dock (kind 2) */
static struct app_item g_drag_item;
static float g_drag_sx, g_drag_sy;   /* press-start pointer */
static float g_drag_x, g_drag_y;     /* live pointer */
static int   g_drag_moved = 0;       /* travelled past the click threshold -> a real drag */
static int   g_any_active = 0;       /* any draggable held this frame */
static float g_dockr[4];             /* dock pill world rect: x0,y0,x1,y1 */
static int   g_have_dockr = 0;
static int   g_dock_dirty = 0;       /* dock changed -> the loop force-repaints (no ghosts) */
/* desktop context menu (right-click on empty desktop) */
static bool  g_ctx_open = false;
static float g_ctx_x, g_ctx_y;

static char  g_pin_exec[16][80];     /* stable exec strings for launcher apps pinned to the dock */
static int   g_pin_n = 0;

/* The top bar (a separate program) asks us to open the launcher over an IPC
 * channel: home LISTENS at /run/emlink.desktop on a background thread (accept
 * blocks, so it can't live in the render loop) and just raises a flag; the top
 * bar CONNECTS to signal. The render loop consumes the flag non-blockingly. */
static volatile int g_apps_requested = 0;
static void apps_listener(long arg) {
    (void)arg;
    int lh = (int)embk_chan_listen("/run/emlink.desktop");
    if (lh < 0) embk_thread_exit(1);
    for (;;) {
        int ch = (int)embk_chan_accept(lh);
        if (ch < 0) {
            /* accept only BLOCKS while the endpoint is healthy; the error paths
             * (bad/unlinked handle, handle table full) return straight away, so
             * a bare retry here would be an unbounded hot spin -- on one core
             * that starves the render loop and the whole desktop freezes. */
            embk_sleep_ms(100);
            continue;
        }
        g_apps_requested = 1;          /* the render loop opens the launcher */
        embk_chan_close(ch);
    }
}
/* Open or close the launcher, and move the desktop LAYER with it.
 *
 * The layer is the ground at z=0, so a launcher drawn into it opens behind
 * every app window -- press the button with anything open and nothing appears
 * to happen, which is exactly what it did. Launchpad is in front, so the layer
 * comes to the front for as long as the launcher is up and goes back down the
 * moment it closes. Everything else about the desktop is unchanged; this is a
 * mode, not a reordering. */
/* Frames still owed a FULL present after the launcher opened or closed. The
 * transition changes every pixel on the layer, and it changes them in two
 * places that do not happen together: the compositor recomposes the moment the
 * z-flip lands, while this loop only renders the new state on its NEXT frame
 * and then presents the dirty rects of it. Anything the new frame does not
 * touch therefore keeps whatever the old one left -- the launcher's title
 * stayed painted across the top of the screen, over the menu bar. A whole-
 * surface present for the frames either side of the flip is what makes the two
 * agree. */
static int g_full_present = 0;

static void apps_set_open(int open) {
    if (open == g_apps_open) return;
    if (open) { scan_apps(); g_apps_frames = 0; g_apps_scroll = 0; g_apps_q[0] = 0; }
    g_apps_open = open;
    /* Render the new state BEFORE moving the layer when closing, and move it
     * before rendering when opening -- either way the compositor's recompose
     * must not be the one that shows a half-updated desktop. */
    embk_win_desktop_front(open);
    g_full_present = 3;
    g_dock_dirty = 1;
}

static void poll_apps_request(void) {
    if (!g_apps_requested) return;
    g_apps_requested = 0;
    /* The menu-bar button TOGGLES, which is what a button that is already lit
     * should do -- pressing it again to dismiss is the first thing anyone
     * tries. */
    apps_set_open(!g_apps_open);
}

static void open_item(struct app_item it) {
    if (it.dir)      launch_folder(it.dir);   /* a folder shortcut -> Files there */
    else if (it.app) g_launch = it.app;       /* an app -> spawn it */
}

/* Adapt a launcher-grid entry to the draggable app_item shape. The exec path
 * points at g_all[]'s reusable buffer; a drop that pins it copies the string to
 * stable storage (dock_resolve_drop), so a later rescan can't alias it. */
static struct app_item grid_to_item(int i) {
    struct app_item it; memset(&it, 0, sizeof it);
    snprintf(it.icon,  sizeof it.icon,  "%s", g_all[i].icon);
    snprintf(it.label, sizeof it.label, "%s", g_all[i].label);
    it.app = g_all[i].exec;
    return it;
}

/* One draggable icon. kind: 1 = desktop source, 2 = dock item, 3 = launcher grid.
 * Renders the icon
 * (with hover styling) and tracks a press as either a click or a drag; the drop
 * is resolved after the frame in dock_resolve_drop(). */
static void drag_icon(struct app_item it, int size, int kind, int idx) {
    /* Key by the app's IDENTITY (its unique path), NOT its slot index. When the
     * list shifts (remove/reorder), an index-keyed instance gets reused for a
     * different app -- which left a removed middle app lingering (dimmed) in
     * place while the pill kept its old width. Identity keys track each app. */
    uint64_t base = kind == 1 ? 0xDE510000ULL : kind == 3 ? 0x6D170000ULL : 0xD0C00000ULL;
    uint64_t key  = base ^ (uint64_t)(uintptr_t)(it.app ? it.app : it.dir);
    ui_box_begin(key);
    (void)ui_open();
    ui_set_align(ALIGN_CENTER);
    /* ALWAYS set opacity, so an instance never stays dimmed after its drag ends */
    ui_set_opacity((g_drag == kind && g_drag_i == idx && g_drag_moved) ? 0.3f : 1.0f);
    ImageButton(it.icon, size);         /* styling only; click/drag handled below */
    if (ui_is_active()) {
        g_any_active = 1;
        float px, py; ui_pointer_pos(&px, &py);
        if (!(g_drag == kind && g_drag_i == idx)) {  /* new press -> start tracking */
            g_drag = kind; g_drag_i = idx; g_drag_item = it;
            g_drag_sx = px; g_drag_sy = py; g_drag_moved = 0;
        }
        g_drag_x = px; g_drag_y = py;
        if (!g_drag_moved) {
            float dx = px - g_drag_sx, dy = py - g_drag_sy;
            if (dx*dx + dy*dy > 49.0f) g_drag_moved = 1;    /* > ~7px = a drag */
        }
    }
    ui_box_end();
}

/* The dock: a centered pill sized to its apps. Captures its own world rect so
 * the drop can hit-test against it. */
static int dock_running(const char *path);   /* below, with g_running */

/* Cursor-driven magnification: an icon's size is a pure function of the
 * pointer's horizontal distance to it -- the Mac dock's defining gesture.
 * No animation clock needed; the hand's own motion IS the animation. Uses
 * LAST frame's dock rect (one-frame-stale geometry is invisible at pointer
 * speeds) and stands down entirely during a drag, where a chip swelling
 * under the ghost would fight the gesture. */
/* The dock's base size is a PREFERENCE, not a constant: Settings writes it and
 * the desktop re-reads it about once a second, so the dock changes under the
 * slider instead of after a reboot. Peak follows base by the same ratio the
 * magnifier was tuned at, so choosing a bigger dock does not also flatten the
 * magnification. */
static struct oscfg g_cfg;
static uint64_t     g_cfg_next = 0;
static void cfg_poll(void) {
    uint64_t now = embk_uptime_ms();
    if (now < g_cfg_next) return;
    g_cfg_next = now + 1000;
    struct oscfg was = g_cfg;
    oscfg_load(&g_cfg);
    /* The shell wears the preference too, and live. em_app_run applies it once
     * at launch for ordinary applications; the desktop never exits, so if it
     * only read the file at startup then changing the accent would recolour
     * every window EXCEPT the one always on screen. */
    if (g_cfg.accent != was.accent || g_cfg.dark != was.dark ||
        g_cfg.ui_scale != was.ui_scale) {
        ui_theme_use_dark(g_cfg.dark != 0);
        ui_theme_set_scale((float)g_cfg.ui_scale / 100.0f);
        const struct oscfg_accent *a = &oscfg_accents[g_cfg.accent];
        ui_theme_set_accent((struct color){ a->r, a->g, a->b, 1.0f });
    }
}
#define DOCK_BASE ((float)g_cfg.dock_size)
#define DOCK_PEAK (DOCK_BASE * 1.53f)
/* Dock band geometry. These were implicit and they DISAGREED: the row reserved
 * 64px while the pill inside it was 70 tall, so the pill overflowed its row
 * and the last 6px -- its bottom edge and rounded corners -- fell off the
 * bottom of the screen. One set of constants now, and the band is the pill
 * PLUS a gap, because a dock that touches the screen edge is a taskbar; the
 * gap is what makes it read as floating. */
#define DOCK_PILL_H (DOCK_BASE + 32.0f)
#define DOCK_GAP    14.0f
/* ONE formula, in oscfg.h, because this number is also what every app window
 * must stay out of -- and when the two disagreed, app windows covered the top
 * of the dock and swallowed its clicks. A contract in a shared header cannot
 * drift the way two copies of a constant did. */
#define DOCK_BAND   ((float)oscfg_dock_band(&g_cfg))
#define BAR_RESERVE 26.0f      /* must match topbar.c's BAR_H */
/* Where slot `i` ACTUALLY starts, derived from the layout rather than guessed.
 *
 * The row is: px 12, then per slot a VStack of width DOCK_BASE+8 separated by
 * spacing 10 -- so the pitch is DOCK_BASE+18, not DOCK_BASE+10. Three places
 * had independently written the +10 version, and the error compounds with the
 * slot index: at the default size it is 8px off at slot 1 and 24px off at slot
 * 3. Proven, not guessed -- the dock's captured world rect is 238px wide for
 * four slots, and 4*(38+8) + 3*10 + 2*12 is exactly 238, while the +10 formula
 * predicts 216.
 *
 * The visible cost: the magnifier swells one icon while you point at another,
 * and the floating label names the wrong app. */
#define DOCK_SLOT_W  (DOCK_BASE + 8.0f)
#define DOCK_PITCH   (DOCK_SLOT_W + 10.0f)
static float dock_slot_x0(int i) { return g_dockr[0] + 12.0f + (float)i * DOCK_PITCH; }

static float dock_icon_size(int i) {
    if (!g_have_dockr || g_drag) return DOCK_BASE;
    float px, py; ui_pointer_pos(&px, &py);
    if (py < g_dockr[1] - 30.0f || py > g_dockr[3] + 10.0f ||
        px < g_dockr[0] - 40.0f || px > g_dockr[2] + 40.0f) return DOCK_BASE;
    float cx = dock_slot_x0(i) + DOCK_SLOT_W * 0.5f;
    float d = px - cx; if (d < 0) d = -d;
    const float radius = 96.0f;
    if (d >= radius) return DOCK_BASE;
    float t = 1.0f - d / radius;
    return DOCK_BASE + (DOCK_PEAK - DOCK_BASE) * t * t;    /* eased falloff */
}

/* Which slot the pointer is over, or -1. Same slot arithmetic as the
 * magnifier, so the label always names the icon that is actually swelling. */
static int dock_hover_index(void) {
    if (!g_have_dockr || g_drag) return -1;
    float px, py; ui_pointer_pos(&px, &py);
    if (py < g_dockr[1] || py > g_dockr[3]) return -1;
    for (int i = 0; i < g_dock_n; i++) {
        float x0 = dock_slot_x0(i);
        if (px >= x0 && px < x0 + DOCK_SLOT_W) return i;
    }
    return -1;
}

/* The name of the icon under the pointer, floating above the dock.
 * Our dock art is abstract -- a glyph is a guess until something names it --
 * and a label that lives IN the dock would either cost permanent vertical
 * space or shift the icons when it appeared. So it is an out-of-flow overlay
 * (the drag-ghost pattern): it costs no layout and moves nothing. */
static void dock_label(void) {
    int i = dock_hover_index();
    if (i < 0 || !g_dock[i].label[0]) return;
    float cx = dock_slot_x0(i) + DOCK_SLOT_W * 0.5f;
    /* rough centring: the toolkit measures the text, we only have its length,
     * and being a few pixels off is invisible next to a 46px icon */
    int n = 0; while (g_dock[i].label[n]) n++;
    float w = (float)n * 7.0f + 20.0f;

    em_flush();                    /* emit pending DSL leaves before raw ui_* */
    ui_begin_vstack(0x70CA);
    ui_set_overlay(true);
    ui_set_layer(1);
    /* ZERO-SIZED, for the reason desktop_icons() spells out: a screen-filling
     * transparent overlay CONTAINS every point, and the hit test gives the
     * highest layer that claims the point. This one only exists WHILE the
     * pointer is over a dock icon -- which is precisely when you are trying to
     * click one -- so at full size it swallowed every launch click. With no
     * extent, only the label's own little box exists for hit testing. */
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
    ui_set_clip_children(false);
    ui_begin_vstack(1);
    ui_set_offset(cx - w * 0.5f, g_dockr[1] - 34.0f);
    HStack(.px = 10, .py = 4, .corner = 9, .glass = 1, .border = 1) {
        Text(g_dock[i].label).caption();
    }
    ui_end_stack();
    ui_end_stack();
}

static void dock_pill(void) {
    /* GLASS, not paint: the pill blurs the wallpaper behind it (the desktop
     * layer paints the wallpaper earlier in this same tree, which is exactly
     * what in-window backdrop blur samples). Bottom-aligned so magnified
     * icons grow UPWARD out of the bar, the way the Mac's do. */
    HStack(.height = DOCK_PILL_H, .spacing = 10, .px = 12, .pb = 6, .align = Trailing,
           .glass = 1, .corner = 20, .border = 1, .shadow = 2) {
        (void)ui_open();
        { float x, y, w, h; g_have_dockr = ui_open_rect(&x, &y, &w, &h);
          if (g_have_dockr) { g_dockr[0]=x; g_dockr[1]=y; g_dockr[2]=x+w; g_dockr[3]=y+h; } }
        for (int i = 0; i < g_dock_n; i++) {
            /* FIXED-width slot: magnification must swell the icon IN PLACE,
             * never reflow the row -- a centered row that re-lays-out as chips
             * grow slides the chip out from under the pointer, so the press
             * you aimed lands in a gap. The icon box overflows its slot
             * upward and sideways (no clip); the slot never moves. */
            VStack(.spacing = 3, .width = DOCK_BASE + 8, .align = Center) {
                drag_icon(g_dock[i], (int)dock_icon_size(i), 2, i);
                /* The running dot: 4px of truth under a live app's chip.
                 *
                 * The paint is set EXPLICITLY in BOTH states, and that is the
                 * whole point. Written as two branches -- a dot with a
                 * .background, and a bare spacer without one -- the reconciler
                 * reuses the same instance (same shape, same position) and
                 * em_apply_box only calls ui_set_paint when .background has
                 * alpha, so the "off" branch set nothing and the instance KEPT
                 * last frame's fill. The dot then survived the app that owned
                 * it: close the terminal and its light stayed on forever. Same
                 * mechanism as the V6 menu smear -- a reused instance retains
                 * every prop the new state fails to set. PAINT_NONE is how you
                 * say "no fill" and mean it. */
                int running = g_cfg.dock_dots && dock_running(g_dock[i].app);
                em_flush();
                /* Key by the app's IDENTITY, exactly as drag_icon does, and for
                 * a sharper reason than tidiness: this key was the CONSTANT
                 * 0xD07 inside the loop over dock slots, so every dot in the
                 * dock claimed the SAME instance. Four emitters, one instance,
                 * every frame.
                 *
                 * That stayed invisible while all the dots were identical --
                 * which is the case until something is running. Launch one app
                 * and its dot alone becomes PAINT_SOLID: now the emitters
                 * disagree about the shared instance, they fight over it frame
                 * after frame, and the dock's reconciled tree -- including the
                 * hit rects of the icons above these dots -- stops matching
                 * what is on screen. From the outside: the dock launches apps
                 * perfectly until you launch one, and then it goes dead, while
                 * the launcher grid and the desktop icons (which have no dots)
                 * keep working. */
                ui_begin_hstack(0xD0700000ULL ^ (uint64_t)(uintptr_t)g_dock[i].app);
                ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 4 },
                            (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 4 });
                ui_set_corner_radius(2);
                struct paint dot = { 0 };            /* PAINT_NONE */
                if (running) {
                    dot.kind = PAINT_SOLID;
                    dot.solid = (Color){ .r=.62f, .g=.66f, .b=.78f, .a=1.f };
                }
                ui_set_paint(dot);
                ui_end_stack();
            }
        }
        if (g_dock_n == 0) { EmProps hp = {0}; (void)hp; Text("  drag apps here  ").caption().secondary(); }
    }
}

/* The ghost: the dragged icon following the cursor (an out-of-flow overlay). */
static void drag_ghost(void) {
    if (!(g_drag && g_drag_moved)) return;
    ui_begin_vstack(0x6057);
    ui_set_overlay(true);            /* fill parent, out of flow */
    ui_set_layer(2);                 /* ABOVE dialog scrims: the ghost must ride
                                      * over the launcher while dragging out of it */
    ui_begin_vstack(1);
    ui_set_offset(g_drag_x - 22.0f, g_drag_y - 22.0f);
    ui_set_opacity(0.85f);
    ImageButton(g_drag_item.icon, 44);
    ui_end_stack();
    ui_end_stack();
}

/* THE APPLICATIONS LAUNCHER -- Launchpad, not a dialog.
 *
 * It used to be a 480px card floating over the desktop, and that was wrong in
 * two ways at once. It read as a file-picker rather than as the place all your
 * applications live; and at 480x430 it did not FIT a 640x480 display, so its
 * header slid up under the menu bar and the app names printed over the clock.
 * A modal card has to be sized against a screen it cannot know.
 *
 * Full-bleed instead: the launcher IS the screen while it is up. The wallpaper
 * home already painted stays visible through a frosted sheet, the grid is
 * centred in the work area, and everything is sized from the live viewport, so
 * there is no resolution at which it can overflow. This is also why the layer
 * comes to the front (see apps_set_open): a full-screen surface behind the
 * windows is just an invisible one. */
/* The tint the launcher's glass carries, and why it carries one at all.
 *
 * "Blur the desktop and dim it" is the whole of this effect on a Mac, and it
 * works there because the wallpaper has colour in it to blur. This OS ships a
 * near-black one, so frosting it lands on black and stays black -- the launcher
 * came out looking like the screen had been switched off, which is a fair
 * description of a dark rectangle with small icons on it.
 *
 * So the glass is tinted rather than merely dimmed: a cool indigo at low alpha,
 * bright enough to give the surface somewhere to catch light and read as a
 * material over a dark room, faint enough that it is still the wallpaper you
 * are looking through. */
static Color launch_tint(void) {
    Color c = { .r = 0.10f, .g = 0.12f, .b = 0.22f, .a = 0.52f };
    return c;
}


/* Does this app match what has been typed? Case-insensitive substring, which
 * is the whole of "search" at this scale: with a dozen apps, ranking is a
 * feature nobody can see the benefit of. */
static int app_matches(const char *label, const char *q) {
    if (!q[0]) return 1;
    for (const char *a = label; *a; a++) {
        const char *x = a, *y = q;
        while (*x && *y) {
            char ca = *x | 32, cb = *y | 32;
            if (ca != cb) break;
            x++; y++;
        }
        if (!*y) return 1;
    }
    return 0;
}

static void apps_grid(void) {
    if (!g_apps_open) return;
    g_apps_frames++;
    const struct ui_theme *t = ui_theme();

    /* Sized from the screen, never from a constant. Columns grow with width so
     * the grid stays a grid rather than a column on a narrow display. */
    float cell   = 116.0f;
    float availw = g_sw - 72.0f;
    if (availw < 240.0f) availw = 240.0f;
    int cols = (int)(availw / cell);
    if (cols < 3) cols = 3;
    if (cols > 7) cols = 7;
    float body = g_sh - 34.0f - 76.0f - 62.0f;    /* minus bar, dock, search row */
    if (body < 120.0f) body = 120.0f;

    Overlay() {
        /* The sheet is the whole screen. Two layers make it read as a material
         * rather than a black rectangle: the frost, and a wash over it.
         *
         * The wash matters more than it sounds. This OS ships a near-black
         * wallpaper, so "blur the desktop and dim it" -- which is the whole of
         * the effect on a Mac -- lands on black and stays black, and the
         * launcher came out looking like the screen had been switched off. A
         * faint accent-tinted gradient gives the surface somewhere to catch
         * light, so it reads as glass over a dark room instead of as an
         * absence. */
        Glass(.width = g_sw, .height = g_sh, .align = Fill, .spacing = 0,
              .pt = 30, .pb = 70, .px = 36, .blur = 30,
              .background = launch_tint()) {
            /* SEARCH, centred, where Launchpad puts it -- and no title. The
             * word "Applications" over a screen of applications is a label for
             * something already obvious, and it was the thing overlapping the
             * menu bar. A search field earns the row: it is the fastest way to
             * start an app once there are more than a screenful. */
            HStack(.align = Center, .justify = Center, .py = 6) {
                HStack(.width = 280, .align = Center) {
                    SearchField(g_apps_q, sizeof g_apps_q, "Search");
                }
            }

            ScrollView(&g_apps_scroll, body, .key = "appsgrid") {
                Grid(cols, .spacing = 10) {
                    for (int i = 0; i < g_all_n; i++) {
                        if (!app_matches(g_all[i].label, g_apps_q)) continue;
                        VStack(.spacing = 9, .align = Center, .py = 8) {
                            /* Big. An app icon is the thing you aim at and the
                             * thing that tells you which app it is, and at 48px
                             * -- the size a DESKTOP icon wants, which is what
                             * this borrowed -- it was doing neither. */
                            drag_icon(grid_to_item(i), 72, 3, i);
                            Text(g_all[i].label).caption().color(t->text);
                        }
                    }
                }
            }
        }
    }
    /* Click anywhere off an icon closes, and so does Esc -- debounced so the
     * press that OPENED it does not immediately dismiss it. */
    if (g_apps_frames >= 3 && OverlayDismissed()) apps_set_open(0);
}

/* --- free desktop placement -------------------------------------------------
 * Desktop icons are positioned individually rather than flowing in a list, so
 * one can be dropped anywhere instead of being pinned to its index. Layout only
 * assigns a spot to icons that have never been placed. */

/* Keep an icon fully inside the work area (below the top bar, above the dock). */
static void desk_clamp(struct app_item *it) {
    float maxx = g_sw - DESK_CELL_W - DESK_MARGIN;
    float maxy = g_sh - 64 - DESK_CELL_H - DESK_MARGIN;
    if (maxx < DESK_MARGIN) maxx = DESK_MARGIN;
    if (maxy < 40) maxy = 40;
    if (it->x < DESK_MARGIN) it->x = DESK_MARGIN;
    if (it->y < 40) it->y = 40;
    if (it->x > maxx) it->x = maxx;
    if (it->y > maxy) it->y = maxy;
}

/* Give any unplaced icon a spot: down the left edge, wrapping into a new
 * column at the bottom -- the arrangement the desktop used to hard-code. */
static void desk_autoplace(void) {
    float x = DESK_MARGIN, y = 48;
    for (int i = 0; i < g_desk_n; i++) {
        if (g_desk[i].placed) continue;
        while (y + DESK_CELL_H > g_sh - 64 - DESK_MARGIN && x + DESK_CELL_W * 2 < g_sw) {
            x += DESK_CELL_W + 8; y = 48;          /* column full -> next column */
        }
        g_desk[i].x = x; g_desk[i].y = y; g_desk[i].placed = 1;
        desk_clamp(&g_desk[i]);
        y += DESK_CELL_H + 18;
    }
}

/* Icon positions outlive the session: a desktop that forgot where everything
 * was on every boot would not be worth arranging. Stored as one line per icon,
 * keyed by the app/folder path (the same stable identity the drag keys use). */
#define DESK_LAYOUT "/data/desktop.layout"

static void desk_save_layout(void) {
    char buf[1600]; int n = 0;
    for (int i = 0; i < g_desk_n && n < (int)sizeof buf - 160; i++) {
        const char *key = g_desk[i].app ? g_desk[i].app : g_desk[i].dir;
        if (!key) continue;
        n += snprintf(buf + n, sizeof buf - n, "%d %d %s\n",
                      (int)g_desk[i].x, (int)g_desk[i].y, key);
    }
    int fd = (int)embk_open(DESK_LAYOUT, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (fd < 0) return;                     /* read-only /data -> just don't persist */
    embk_write(fd, buf, (size_t)n);
    embk_close(fd);
}

static void desk_load_layout(void) {
    size_t len = 0;
    uint8_t *b = read_file(DESK_LAYOUT, &len);
    if (!b) return;
    for (size_t i = 0; i < len; ) {
        size_t ls = i;
        while (i < len && b[i] != '\n') i++;
        size_t le = i; if (i < len) i++;
        char line[256];
        size_t ll = le - ls; if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, b + ls, ll); line[ll] = 0;
        int x = 0, y = 0; char key[200];
        if (sscanf(line, "%d %d %199[^\n]", &x, &y, key) != 3) continue;
        for (int k = 0; k < g_desk_n; k++) {
            const char *kk = g_desk[k].app ? g_desk[k].app : g_desk[k].dir;
            if (kk && !strcmp(kk, key)) {
                g_desk[k].x = (float)x; g_desk[k].y = (float)y; g_desk[k].placed = 1;
                desk_clamp(&g_desk[k]);
                break;
            }
        }
    }
    free(b);
}

/* Resolve a released drag: add to / remove from / reorder the dock. Called after
 * the frame is built, when the dock rect and final pointer are known. */
static void dock_resolve_drop(void) {
    if (!g_drag || g_any_active) return;         /* still held, or nothing pressed */
    int over = g_have_dockr &&
               g_drag_x >= g_dockr[0] - 12 && g_drag_x <= g_dockr[2] + 12 &&
               g_drag_y >= g_dockr[1] - 28 && g_drag_y <= g_dockr[3] + 12;
    if (!g_drag_moved) {
        open_item(g_drag_item);                   /* a plain click -> launch/open */
        if (g_drag == 3) apps_set_open(0);        /* grid click closes launcher */
    } else if (g_drag == 3) {                      /* launcher app dragged toward the dock */
        /* pin a COPY: the grid's exec buffer gets reused on the next rescan, so
         * the dock keeps its own stable exec string. Skip if already docked. */
        int dup = 0;
        for (int k = 0; k < g_dock_n; k++)
            if (g_dock[k].app && g_drag_item.app && !strcmp(g_dock[k].app, g_drag_item.app)) dup = 1;
        if (over && !dup && g_dock_n < 16 && g_pin_n < 16) {
            char *buf = g_pin_exec[g_pin_n++];
            snprintf(buf, sizeof g_pin_exec[0], "%s", g_drag_item.app);
            g_drag_item.app = buf;
            g_dock[g_dock_n++] = g_drag_item;
            apps_set_open(0);                      /* pinned -> close the launcher */
            g_dock_dirty = 1;
        }                                          /* dropped off the dock -> launcher stays open */
    } else if (g_drag == 1) {                      /* a desktop icon */
        if (over && g_dock_n < 16) {
            g_dock[g_dock_n++] = g_drag_item;      /* onto the dock -> pin it ... */
            for (int k = g_drag_i; k < g_desk_n - 1; k++) g_desk[k] = g_desk[k+1];
            g_desk_n--;                            /* ... and REMOVE from the desktop */
            g_dock_dirty = 1;
            desk_save_layout();
        } else if (!over && g_drag_i >= 0 && g_drag_i < g_desk_n) {
            /* dropped on open desktop -> it LIVES there now. Centre the cell on
             * the cursor so the icon lands where the pointer let go, not offset
             * by wherever inside the icon the drag happened to start. */
            g_desk[g_drag_i].x = g_drag_x - DESK_CELL_W / 2.0f;
            g_desk[g_drag_i].y = g_drag_y - DESK_ICON / 2.0f;
            g_desk[g_drag_i].placed = 1;
            desk_clamp(&g_desk[g_drag_i]);
            g_dock_dirty = 1;
            desk_save_layout();
        }
    } else if (g_drag == 2) {                       /* dock item */
        if (!over) {                                /* pulled out -> back to the desktop */
            if (g_dock_n > DOCK_MIN && g_desk_n < 16) {   /* keep at least DOCK_MIN docked */
                g_drag_item.x = g_drag_x - DESK_CELL_W / 2.0f;   /* land where dropped */
                g_drag_item.y = g_drag_y - DESK_ICON / 2.0f;
                g_drag_item.placed = 1;
                desk_clamp(&g_drag_item);
                g_desk[g_desk_n++] = g_drag_item;  /* return it to the desktop ... */
                for (int k = g_drag_i; k < g_dock_n - 1; k++) g_dock[k] = g_dock[k+1];
                g_dock_n--;                        /* ... and remove from the dock */
                g_dock_dirty = 1;
                desk_save_layout();
            }                                       /* at the minimum -> snaps back */
        } else if (g_dockr[2] > g_dockr[0]) {       /* reorder by x */
            int tgt = (int)(((g_drag_x - g_dockr[0]) / (g_dockr[2] - g_dockr[0])) * g_dock_n);
            if (tgt < 0) tgt = 0;
            if (tgt >= g_dock_n) tgt = g_dock_n - 1;
            if (tgt != g_drag_i) {
                struct app_item tmp = g_dock[g_drag_i];
                if (tgt > g_drag_i) for (int k=g_drag_i; k<tgt; k++) g_dock[k]=g_dock[k+1];
                else                for (int k=g_drag_i; k>tgt; k--) g_dock[k]=g_dock[k-1];
                g_dock[tgt] = tmp; g_dock_dirty = 1;
            }
        }
    }
    g_drag = 0; g_drag_i = -1; g_drag_moved = 0;
}

/* Desktop icons, each drawn at its OWN position rather than flowing in a list.
 * One out-of-flow overlay spans the desktop and every icon is offset inside it,
 * which is what lets a drop put an icon anywhere instead of returning it to a
 * slot decided by its index. (Same mechanism the drag ghost uses.) */
static void desktop_icons(void) {
    desk_autoplace();
    em_flush();                       /* emit pending DSL leaves before raw ui_* */
    ui_begin_vstack(0xDE5C0DEEULL);
    ui_set_overlay(true);
    /* A ZERO-SIZED root, not a screen-filling one. The icons are positioned by
     * per-child offsets, which need no parent extent -- and a full-screen
     * transparent root CONTAINS every point, so wherever this overlay sits in
     * document order it either eats the dock's clicks (declared after it) or
     * loses the icons' clicks to the work-area strip (declared before it).
     * With no extent of its own, only the ICON BOXES exist for hit testing:
     * each wins exactly its own pixels and nothing else, and declaration order
     * stops mattering at all. That dilemma was this desktop's whole click-bug
     * history in one node. */
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
    ui_set_clip_children(false);
    for (int i = 0; i < g_desk_n; i++) {
        const void *id = g_desk[i].app ? (const void *)g_desk[i].app
                                       : (const void *)g_desk[i].dir;
        ui_begin_vstack(0xDE5C0000ULL ^ (uint64_t)(uintptr_t)id);
        ui_set_offset(g_desk[i].x, g_desk[i].y);
        VStack(.spacing = 5, .width = DESK_CELL_W, .align = Center) {
            drag_icon(g_desk[i], DESK_ICON, 1, i);
            Text(g_desk[i].label).caption();
        }
        em_flush();
        ui_end_stack();
    }
    ui_end_stack();
}

/* Post-login desktop: wallpaper, three honest folder shortcuts, and a
 * full-width bottom taskbar. The right side deliberately exposes only state the
 * OS can report today; network/audio/battery indicators arrive with their
 * corresponding services instead of being decorative lies. */
static void home_ui(void) {
    g_any_active = 0; g_have_dockr = 0;   /* recomputed each frame during the build */
    Screen(.width = g_sw, .height = g_sh, .padding = -1, .align = Fill) {
        BackgroundImage("/system/images/colibri-user.ppm");
        VStack(.width = g_sw, .height = g_sh, .padding = 0, .spacing = 0,
               .align = Fill) {
            /* Reserve the top strip for our own floating menu bar (topbar.elf,
             * spawned at startup) -- its drag handle, dock and pin live there.
             * No desktop-drawn top bar anymore. */
            VStack(.width = g_sw, .height = BAR_RESERVE, .padding = 0) { }

            /* Exact work-area height: the layout engine's `.grow` is intended
             * for siblings inside a measured row and did not consume the
             * remaining desktop height here, which left the taskbar mid-screen.
             * The icons themselves no longer flow inside it -- they are placed
             * individually by desktop_icons() -- but the strip still reserves
             * the space between the top bar and the dock. */
            HStack(.height = g_sh - BAR_RESERVE - DOCK_BAND) { Spacer(); }

            /* the app dock: a centered pill sized to its apps (not full-width) */
            /* Leading (= top of the band): the pill sits at the top of its
             * band and the gap lands UNDER it, against the screen edge. */
            HStack(.width = g_sw, .height = DOCK_BAND, .align = Leading, .justify = Center) {
                dock_pill();
            }
        }
        desktop_icons();       /* freely-placed icons; zero-sized root, so it
                                * can sit last without shadowing anything */

        /* Right-click the desktop. Every item does something REAL and
         * immediate -- no greyed-out placeholders, which are just an apology
         * in menu form. "Clean Up" exists because free placement earned it:
         * once icons can go anywhere, you need one gesture to make them tidy
         * again. */
        {
            float rx, ry;
            if (RightClicked(&rx, &ry)) {
                g_ctx_x = rx; g_ctx_y = ry; g_ctx_open = true;
                g_dock_dirty = 1;
            }
        }
        ContextMenu(&g_ctx_open, g_ctx_x, g_ctx_y) {
            if (MenuItem("New Terminal")) g_launch = "/data/apps/term/term.elf";
            if (MenuItem("Open Files"))   launch_folder(getenv("HOME") ? getenv("HOME") : "/");
            MenuSeparator();
            if (MenuItem("Show Applications")) apps_set_open(1);
            if (MenuItem("Clean Up Icons")) {
                /* forget every position; autoplace re-columns them next frame */
                for (int i = 0; i < g_desk_n; i++) g_desk[i].placed = 0;
                desk_save_layout();
                g_dock_dirty = 1;
            }
        }
        apps_grid();           /* the Apps launcher (modal grid), when open */
        dock_label();          /* names the dock icon under the pointer (overlay) */
        drag_ghost();          /* the dragged icon follows the cursor (overlay) */
    }
    dock_resolve_drop();       /* act on a released drag (add / remove / reorder) */
}

/* One instance per app: remember each child's spawn HANDLE (what embk_spawn
 * returns -- NOT a pid; handles are 0-based, stored here +1 so the zeroed
 * static table means "none") and refuse to spawn again while that child is
 * alive. Once it has exited or been closed, embk_wait() the dead child BEFORE
 * respawning: that reaps its zombie process slot AND frees the spawn handle.
 * Without the wait, every launch leaked one of the 16 per-process handles and
 * the 17th spawn failed -- killing its own child on the spot; and the old code
 * stored the handle AS a pid, so proc_alive() interrogated some unrelated
 * always-alive low pid (the shell/idle) and refused every relaunch. */
#define MAX_TRACKED 8
static struct {
    char path[96];          /* exec path, COPIED -> a stable identity even when the
                             * caller's pointer is a reused scan buffer (grid apps) */
    char start_dir[256];    /* "" = none; distinguishes independent Files shortcuts */
    int  used;
    int  handle_p1;
} g_running[MAX_TRACKED];

/* Is the app behind this dock chip alive right now? Drives the dock's
 * indicator dot, so a dot is a RUNNING process, not a memory of one
 * (proc_alive interrogates the spawn handle; dead handles read 0). */
static int dock_running(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_running[i].used && g_running[i].handle_p1 > 0 &&
            strcmp(g_running[i].path, path) == 0)
            return embk_proc_alive(g_running[i].handle_p1 - 1) == 1;
    return 0;
}

/* An app declares the authority it needs in two sidecars beside its binary --
 * <name>.ns (what it may NAME) and <name>.caps (what it may DO). The parsing
 * lives in user/lib/appauth.c because reading an app's declaration is not the
 * desktop's business specifically; any launcher asks the same question.
 * See docs/USERSPACE_v2.md UP4. */
#define NS_ACTS_MAX 8

static void spawn_app(const char *path, const char *start_dir) {
    const char *sd = start_dir ? start_dir : "";
    /* Dedup by exec+dir CONTENT, not pointer: the same app reached from the dock
     * (a string literal) and from the launcher grid (a reused scan buffer) has
     * two different addresses -- pointer-equality let it spawn TWICE, and two
     * instances fighting over one window rendered as a dead black rectangle. */
    int slot = -1, freeslot = -1, deadslot = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_running[i].used) {
            if (!strcmp(g_running[i].path, path) &&
                !strcmp(g_running[i].start_dir, sd)) { slot = i; break; }
            /* A slot whose app has EXITED is reusable. `used` was never
             * cleared once set, so after eight distinct apps this table was
             * permanently full and every further launch returned silently --
             * apps simply stopped starting, with no dot and no error. The
             * table is a cache of what is running, not a registry of what has
             * ever run. */
            if (deadslot < 0 && (g_running[i].handle_p1 <= 0 ||
                                 !embk_proc_alive(g_running[i].handle_p1 - 1)))
                deadslot = i;
        } else if (freeslot < 0) freeslot = i;
    }

    if (slot >= 0) {                       /* already tracked */
        if (g_running[slot].handle_p1 > 0) {
            int h = g_running[slot].handle_p1 - 1;
            if (embk_proc_alive(h)) {
                /* Still running -- one instance only. But "the app is already
                 * open" is not a reason to do NOTHING: that is what made a dock
                 * click on a running app feel broken, and it is the only way
                 * back for a window the user minimized. Bring it forward. */
                embk_win_restore(h);
                return;
            }
            embk_wait(h);                     /* dead: reap the zombie + free the handle */
            g_running[slot].handle_p1 = 0;
        }
    } else {
        slot = freeslot >= 0 ? freeslot : deadslot;   /* evict an exited app */
        if (slot < 0) return;              /* genuinely full: all still running */
        if (g_running[slot].used && g_running[slot].handle_p1 > 0)
            embk_wait(g_running[slot].handle_p1 - 1);   /* reap before reusing */
        g_running[slot].handle_p1 = 0;
    }

    /* Grant the child EXACTLY its declared namespace (UP4); no manifest => it
     * inherits our full view. */
    struct embk_spawn_file_action acts[NS_ACTS_MAX + 1];
    char nsdesc[224], capdesc[128];
    int nacts = appauth_load_ns(path, acts, NS_ACTS_MAX, nsdesc, sizeof nsdesc);

    /* ...and the capability half of the same declaration. A SET_CAPS action
     * goes FIRST so the log reads in the order authority is decided, though the
     * kernel applies each action independently. An app with no .caps inherits
     * ours, exactly as an app with no .ns inherits our namespace -- silence is
     * the pre-manifest behaviour, and only a manifest can narrow. */
    unsigned capmask = 0;
    int have_caps = appauth_load_caps(path, &capmask, capdesc, sizeof capdesc);
    if (have_caps && nacts < NS_ACTS_MAX + 1) {
        /* shift the ns binds along to keep SET_CAPS first */
        for (int k = nacts; k > 0; k--) acts[k] = acts[k - 1];
        embk_action_set_caps(&acts[0], capmask);
        nacts++;
    }

    char *argv[] = { (char *)path, NULL };
    char file_path_env[640];
    char *launch_env[64];
    char **env = g_session_env;
    if (start_dir && start_dir[0] == '/') {
        int en = 0;
        while (g_session_env && g_session_env[en] && en < 62) {
            launch_env[en] = g_session_env[en];
            en++;
        }
        snprintf(file_path_env, sizeof file_path_env, "FILES_PATH=%s", start_dir);
        launch_env[en++] = file_path_env;
        launch_env[en] = NULL;
        env = launch_env;
    }
    int h = (int)embk_spawn_env(path, argv, env, nacts ? acts : NULL, nacts);

    char b[448];
    int binds = nacts - (have_caps ? 1 : 0);
    if (binds || have_caps)
        snprintf(b, sizeof b, "home: spawn %s -> ns[%s] caps[%s]\n", path,
                 binds ? nsdesc : "inherit", have_caps ? capdesc : "inherit");
    else
        snprintf(b, sizeof b, "home: spawn %s -> full inherit (no manifest)\n", path);
    embk_puts(1, b);

    if (h >= 0) {
        snprintf(g_running[slot].path, sizeof g_running[slot].path, "%s", path);
        snprintf(g_running[slot].start_dir, sizeof g_running[slot].start_dir, "%s", sd);
        g_running[slot].used = 1;
        g_running[slot].handle_p1 = h + 1;
    }
    else { char e[96]; snprintf(e, sizeof e, "home: spawn %s FAILED: %d\n", path, h); embk_puts(1, e); }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    g_session_env = envp;
    embk_thread_create(apps_listener, 0);   /* the top bar's Apps signal listener */
    /* apps describe their own icon/name (docs presentation manifest) */
    load_app_meta("files", g_dock[0].icon, sizeof g_dock[0].icon, g_dock[0].label, sizeof g_dock[0].label);
    load_app_meta("term",  g_dock[1].icon, sizeof g_dock[1].icon, g_dock[1].label, sizeof g_dock[1].label);
    /* toolkit font + context */
    size_t rl = 0;
    uint8_t *reg = read_file("/system/fonts/font.ttf", &rl);
    uint32_t fr = reg ? font_load(reg, rl) : 0;
    if (fr) font_install_backend();
    embk_puts(1, fr ? "home: font loaded\n" : "home: FONT MISSING\n");

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    ui_theme_use_dark(true);
    ui_init(&sa, &la);
    em_res_set_loader(read_file);

    /* Take the full screen as the compositor's desktop layer (zero-copy): the
     * toolkit renders its launcher straight into the shared pages. */
    void *pixels = 0; uint32_t sw = 0, sh = 0;
    int win = embk_win_create_desktop(&pixels, &sw, &sh);
    if (win < 0 || !pixels || sw == 0 || sh == 0) {
        embk_puts(1, "home: desktop create FAILED\n");
        return 1;
    }
    g_sw = (float)sw; g_sh = (float)sh;   /* home_ui sizes the Screen to these */
    /* ...and TELL THE TOOLKIT, which em_app_run would have done for us. em
     * sizes popover dismiss-scrims to the viewport, so an unset one leaves a
     * 0x0 scrim: the context menu would open and then refuse to be dismissed
     * by clicking away from it. */
    em_set_viewport(g_sw, g_sh);
    /* Restore where the user last left each icon. Must run AFTER the screen
     * size is known: the loader clamps every position into the work area, and
     * clamping against a 0x0 screen would collapse the whole desktop into one
     * corner. */
    desk_load_layout();

    struct render_target rt = { (uint32_t *)pixels, sw, sh, sw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());

    em_set_clock(embk_uptime_ms);
    /* Our menu bar IS topbar.elf -- the Apple-modern glass bar with the drag
     * handle, draggable dock chips, and pin/snap. The desktop no longer draws
     * its own top status bar; this floating bar takes the top strip. */
    spawn_app("/data/apps/topbar/topbar.elf", NULL);
    embk_puts(1, "home: desktop ready\n");

    for (;;) {
        cfg_poll();            /* dock size / indicator, as Settings left them */
        poll_apps_request();   /* the top bar's Apps button opens our launcher */

        /* Esc closes the launcher. Only worth reading while it is up: the
         * desktop layer is in FRONT then, so the compositor gives it the
         * keyboard, and draining keys at any other time would take them from
         * whichever app the user is actually typing into. */
        if (g_apps_open) {
            for (int c; (c = embk_key_poll()) > 0; ) {
                if (c == 27) { apps_set_open(0); break; }   /* Esc dismisses */
                /* Everything else goes to the search field. The desktop has no
                 * other text to type into, so there is nothing to arbitrate
                 * between -- and typing straight into the search box, without
                 * clicking it first, is most of why searching a launcher is
                 * faster than looking through it. */
                ui_input_char(c);
            }
        }

        /* pointer: the compositor routes the desktop's content-local mouse to us */
        struct embk_win_input in;
        embk_win_input(&in);
        /* the SAME feed em_app_run uses -- home has its own loop (it owns the
         * back-pinned desktop layer, which the app runtime does not create),
         * but it must never have its own idea of what the toolkit needs. */
        em_feed_pointer((float)in.x, (float)in.y,
                        in.buttons & EMBK_MOUSE_LEFT, in.buttons & EMBK_MOUSE_RIGHT,
                        in.wheel, in.focused);

        /* live uptime clock in the header */
        uint64_t secs = embk_uptime_ms() / 1000;
        snprintf(g_clock, sizeof g_clock, "up %lu:%02lu",
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm) strftime(g_datetime, sizeof g_datetime, "%a %d %b  %H:%M", tm);

        g_launch = 0;
        g_launch_dir = 0;
        ui_frame_begin(); em_new_frame(); home_ui(); em_flush(); ui_frame_end();
        ui_run_layout((float)sw, (float)sh);

        /* No forced full repaints. This loop used to nuke the renderer's rect
         * cache (two whole frames' worth) on every dock change and all through
         * a drag, because the dirty tracker lost the pixels of MOVED and
         * DESTROYED nodes -- the closed launcher stayed painted, dragged icons
         * left trails. That hole is fixed where it belonged, in scene_render
         * (moved nodes union old+new footprints; vacated slots -- Step 1b --
         * surface their last footprint from the renderer's cache), so partial
         * repaints are simply always correct now, and cost what changed. */
        g_dock_dirty = 0;

        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        if (r.full || r.n_dirty == 0 || g_full_present > 0) {
            if (g_full_present > 0) g_full_present--;
            embk_win_present(win, pixels, sw, sh);
        } else {
            int x0 = 1 << 29, y0 = 1 << 29, x1 = -(1 << 29), y1 = -(1 << 29);
            if (r.has_scroll_present) {           /* a scroll blit moved pixels */
                x0 = (int)r.sp_x; y0 = (int)r.sp_y;
                x1 = (int)(r.sp_x + r.sp_w) + 1;
                y1 = (int)(r.sp_y + r.sp_h) + 1;
            }
            for (int i = 0; i < r.n_dirty; i++) {
                int a = (int)r.dirty[i].x, b = (int)r.dirty[i].y;
                int c = (int)(r.dirty[i].x + r.dirty[i].w) + 1, d = (int)(r.dirty[i].y + r.dirty[i].h) + 1;
                if (a < x0) x0 = a;
                if (b < y0) y0 = b;
                if (c > x1) x1 = c;
                if (d > y1) y1 = d;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int)sw) x1 = (int)sw;
            if (y1 > (int)sh) y1 = (int)sh;
            if (x1 > x0 && y1 > y0)
                embk_win_present_rect(win, pixels, sw, sh, x0, y0, x1 - x0, y1 - y0);
        }

        /* a tile was clicked this frame -> launch it as a floating window */
        if (g_launch) spawn_app(g_launch, g_launch_dir);

        embk_sleep_ms(15);   /* pace ~60Hz while YIELDING -- never starve the apps */
    }
    return 0;
}
