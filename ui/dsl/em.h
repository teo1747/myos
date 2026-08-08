#ifndef __EMBLINK_EM_UI_H__
#define __EMBLINK_EM_UI_H__

/* ui/dsl/em.h -- EmUI V2: a SwiftUI-flavored declarative DSL for EmbLink.
 *
 * A thin macro + function layer over the immediate-mode core (ui/declare) that
 * makes app UI read like SwiftUI:
 *
 *     VStack(.spacing = 12, .padding = 20, .background = T.surface, .corner = 16) {
 *         HStack(.align = Center) {
 *             Text("Settings", .font = Title);
 *             Spacer();
 *             Badge("Pro", .tone = Success);
 *         }
 *         Divider();
 *         Toggle("Dark mode", &g_dark);
 *         Slider(&g_volume);
 *         if (Button("Save changes", .style = Primary)) save();
 *     }
 *
 * How the syntax works in C:
 *   - Containers are brace scopes via a for-loop guard (VStack { ... }).
 *   - Modifiers are C designated initializers into one EmProps struct -- the
 *     direct analog of SwiftUI's named arguments. 0 means "unset -> default".
 *   - Views bind to state through pointers (&g_dark), SwiftUI's $binding.
 *
 * RULE: do NOT `return`, `break`, `continue`, or `goto` out of a container
 * block -- it skips the matching close and unbalances the tree (same caveat as
 * every C UI-scope macro). Structure views so control flow stays inside. */

#include "ui.h"
#include "theme.h"
#include <stddef.h>
#include <stdint.h>

typedef struct color Color;

/* ------------------------------------------------------------------------- */
/* enums (short, SwiftUI-like names; 0 == default)                           */
/* ------------------------------------------------------------------------- */

typedef enum {                 /* .font = Title / Body / ... */
    FontDefault = 0, Body, BodyBold, Title, Heading, Caption,
    /* Title size at REGULAR weight. Title and Heading are both hardwired to
     * the bold face, which is right for headings and wrong for everything
     * else that is merely large -- a lead paragraph, a quiet stat. CSS made
     * the gap obvious (`font-size: 19px` with no `font-weight` came out bold),
     * but it was always there. Appended, so every existing value is unchanged. */
    Subtitle
} EmFont;

typedef enum {                 /* .align / .justify (shared) */
    AlignDefault = 0, Leading, Center, Trailing, Fill, SpaceBetween
} EmAlign;

typedef enum {                 /* .style on Button */
    StyleDefault = 0, Primary, Secondary, Ghost, Destructive
} EmStyle;

typedef enum {                 /* .tone on Badge / Tag / Banner */
    ToneDefault = 0, Accent, Success, Warning, Danger, Neutral
} EmTone;

/* ------------------------------------------------------------------------- */
/* EmProps -- every view accepts these as designated-init modifiers.          */
/* A zero field means "not set"; the view falls back to a sensible default.   */
/* ------------------------------------------------------------------------- */

/* An EXPLICIT zero for a length prop, where a plain 0 means "unset -- use
 * the theme's value". See the note inside EmProps. */
#define EmZero (-1.0f)

typedef struct {
    /* layout */
    float spacing;                       /* gap between children */
    float padding, px, py;               /* all / horizontal / vertical */
    float pt, pr, pb, pl;                /* per-edge overrides */
    float width, height;                 /* fixed size (0 = intrinsic) */
    float minw, maxw, minh, maxh;        /* size bounds (0 = unset); maxw + grow
                                          * = a clamped-responsive box */
    int   grow;                          /* fill available space (main axis) */
    int   span;                          /* grid child: columns to span (default 1) */

    /* surface */
    Color background, color, border_color;
    float border, corner;
    int   shadow;                        /* 0 none, 1 sm, 2 md, 3 lg */
    float opacity;                       /* 0 -> treated as 1 */
    int   clip;                          /* clip children to bounds */
    int   glass;                         /* frosted backdrop-blur material */
    float blur;                          /* glass blur radius (0 -> theme default) */

    /* text */
    EmFont font;

    /* alignment */
    EmAlign align, justify;

    /* variants */
    EmStyle style;
    EmTone  tone;

    /* Sizes and spacings below use 0 to mean "unset -- take the theme's
     * value", which is what makes designated initialisers pleasant to write
     * and leaves no way at all to ask for ZERO. Pass EmZero when zero is what
     * you mean.
     *
     * It is not a hypothetical gap: a browser sets its link words with no
     * horizontal padding, because the word already carries the space that
     * followed it in the source. Asking for .px(0) silently got the theme's
     * 16px instead and every linked headline came out visibly gappy -- which
     * looked like a text-metrics bug and was an API that could only override
     * upwards. */

    /* RECONCILIATION KEY. Optional, and only matters for a container whose
     * PRESENCE varies -- a bar that appears when you open it, a row that shows
     * up when there are two of something.
     *
     * Instances are otherwise matched to last frame's by POSITION, which is
     * right until a row is inserted: then every sibling after it adopts its
     * neighbour's retained instance, and since a reused instance keeps the
     * size nobody restated, the whole panel comes back wearing the wrong
     * geometry. (This is exactly what a browser's find bar and tab strip did:
     * one appeared and the rows below it were laid out as each other.)
     *
     * A key makes the match by identity instead. Give one to every child of a
     * container that some sibling can be inserted into -- the STABLE rows as
     * much as the optional one, since it is the stable rows that get displaced.
     *
     * ADDED AT THE END on purpose: EmProps is passed by value across
     * libembk.so, and a field inserted in the middle silently reinterprets
     * every positional initializer that already exists. */
    const char *key;
} EmProps;

/* ------------------------------------------------------------------------- */
/* design tokens -- `T.accent`, `T.text`, ... resolve from the active theme.  */
/* ------------------------------------------------------------------------- */

typedef struct {
    Color accent, accent_soft, on_accent;
    Color text, secondary, tertiary;
    Color surface, surface_alt, bg;
    Color border, border_strong;
    Color success, warning, danger;
    Color clear;                         /* transparent */
} EmTokens;
const EmTokens *em_tokens_(void);
#define T (*em_tokens_())

/* ------------------------------------------------------------------------- */
/* the brace-scope guard: `Container(...) { children }`                       */
/* ------------------------------------------------------------------------- */

#define EM_CAT_(a, b) a##b
#define EM_CAT2_(a, b) EM_CAT_(a, b)
#define EM_SCOPE_(open, close) \
    for (int EM_CAT2_(_em_, __LINE__) = ((open), 0); \
         EM_CAT2_(_em_, __LINE__) == 0; \
         EM_CAT2_(_em_, __LINE__) = ((close), 1))

/* ------------------------------------------------------------------------- */
/* containers (brace-scoped)                                                  */
/* ------------------------------------------------------------------------- */

#define VStack(...) EM_SCOPE_(em_vstack_((EmProps){__VA_ARGS__}), em_end_())
#define HStack(...) EM_SCOPE_(em_hstack_((EmProps){__VA_ARGS__}), em_end_())
#define ZStack(...) EM_SCOPE_(em_zstack_((EmProps){__VA_ARGS__}), em_end_())
#define Card(...)   EM_SCOPE_(em_card_((EmProps){__VA_ARGS__}),  em_end_())
/* Glass: a frosted panel -- blurs whatever is behind it (backdrop blur),
 * tinted with the theme surface + a hint of the EmbLink accent, and finished
 * with a light edge highlight for depth. Equivalent to any container with
 * `.glass = 1` (optionally `.blur = <radius>`). Use for chrome, menus, sheets. */
#define Glass(...)  EM_SCOPE_(em_glass_((EmProps){__VA_ARGS__}),  em_end_())
#define Screen(...) EM_SCOPE_(em_screen_((EmProps){__VA_ARGS__}),em_end_())
#define Section(title, ...) EM_SCOPE_(em_section_((title), (EmProps){__VA_ARGS__}), em_end_())
#define ScrollView(bind, height, ...) EM_SCOPE_(em_scroll_((bind), (height), (EmProps){__VA_ARGS__}), em_scroll_end_())

/* Flow: a horizontal stack whose children WRAP onto new lines when they overflow
 * the width (real flex-wrap in the layout engine -- e.g. chips, tag lists). */
void em_flow_(EmProps p);
#define Flow(...) EM_SCOPE_(em_flow_((EmProps){__VA_ARGS__}), em_end_())

/* Dock: a drag-to-REORDER + drag-out-to-REMOVE row of chips (EmUI drag-and-drop).
 * `ids` = the display order (length *n); render(id) draws one chip's content.
 * Drag a chip to reorder; pull it below the row and release to remove it. */
void em_dock(int *ids, int *n, void (*render)(int id), EmProps p);
int  em_dock_dragging(void);   /* id currently being dragged, or -1 */
#define Dock(ids, n, render, ...) em_dock((ids), (n), (render), (EmProps){__VA_ARGS__})

/* Grid: a true 2D grid -- `cols` equal columns, children auto-flow with an
 * optional per-child .span. Fills the parent width; row heights auto-size to
 * content. Gaps come from .spacing.   Grid(3, .spacing=12){ Card(.span=2){} ...} */
void em_grid_(int cols, EmProps p);
#define Grid(cols, ...) EM_SCOPE_(em_grid_((cols), (EmProps){__VA_ARGS__}), em_end_())

/* Gradient borders (render-engine feature): stroke a container's border with a
 * linear gradient. em_lgrad/em_lgrad3 build the paint; GradientBorder wraps
 * content in a box whose border is that gradient.
 *   GradientBorder(2.0f, em_lgrad(t->accent, pink, 45), .corner=16, .padding=16){...} */
struct paint em_lgrad(Color a, Color b, float angle_deg);
struct paint em_lgrad3(Color a, Color b, Color c, float angle_deg);
void em_gborder_(float width, struct paint g, EmProps p);
void em_gborder_end_(void);
#define GradientBorder(width, grad, ...) \
    EM_SCOPE_(em_gborder_((width), (grad), (EmProps){__VA_ARGS__}), em_gborder_end_())

/* Gradient-filled text / icon (e.g. a gradient heading). */
void em_gtext(const char *s, struct paint g, EmProps p);
void em_gicon(int cp, struct paint g, EmProps p);
#define GradientText(s, grad, ...)  em_gtext((s), (grad), (EmProps){__VA_ARGS__})
#define GradientIcon(cp, grad, ...) em_gicon((cp), (grad), (EmProps){__VA_ARGS__})

void em_vstack_(EmProps p);
void em_hstack_(EmProps p);
void em_zstack_(EmProps p);
void em_card_(EmProps p);
void em_glass_(EmProps p);
void em_screen_(EmProps p);
void em_section_(const char *title, EmProps p);
void em_scroll_(float *scroll_y, float viewport_h, EmProps p);
void em_scroll_end_(void);
void em_end_(void);

#define NavBar(title, ...)  EM_SCOPE_(em_navbar_((title), (EmProps){__VA_ARGS__}), em_end_())
#define Row(...)            EM_SCOPE_(em_row_((EmProps){__VA_ARGS__}), em_end_())
void em_navbar_(const char *title, EmProps p);
void em_row_(EmProps p);

/* ------------------------------------------------------------------------- */
/* leaves + controls -- CHAINABLE. A leaf stages a pending element and returns  */
/* the chain object; modifiers mutate it; it is emitted at the next boundary.   */
/*                                                                              */
/*     Text("Hello").caption().secondary();                                     */
/*     if (Button("Save").primary().clicked()) save();                          */
/*     Button("Delete").destructive().id("del");   // + if (Clicked("del"))     */
/* ------------------------------------------------------------------------- */

typedef struct EmV EmV;
struct EmV {
    /* type roles (text) */
    EmV (*title)(void);   EmV (*heading)(void); EmV (*body)(void);
    EmV (*bold)(void);    EmV (*caption)(void); EmV (*font)(EmFont);
    /* colour -- kind-aware (text colour, or a control's variant/tone) */
    EmV (*color)(Color);
    EmV (*secondary)(void); EmV (*tertiary)(void); EmV (*accent)(void);
    EmV (*primary)(void);   EmV (*ghost)(void);    EmV (*destructive)(void);
    EmV (*tone)(EmTone);    EmV (*success)(void);  EmV (*warning)(void); EmV (*danger)(void);
    /* box */
    EmV (*bg)(Color);     EmV (*padding)(float); EmV (*px)(float); EmV (*py)(float);
    EmV (*frame)(float, float); EmV (*width)(float); EmV (*height)(float); EmV (*grow)(void);
    EmV (*corner)(float); EmV (*border)(float);  EmV (*shadow)(int);
    EmV (*center)(void);  EmV (*leading)(void);  EmV (*trailing)(void); EmV (*align)(EmAlign);
    /* identity + interaction terminals */
    EmV  (*id)(const char *);
    bool (*clicked)(void);
    bool (*focused)(void);
};

#define Text(...)        em_text(__VA_ARGS__)
#define Icon(...)        em_icon(__VA_ARGS__)
#define Label(...)       em_label(__VA_ARGS__)
#define Badge(...)       em_badge(__VA_ARGS__)
#define Tag(...)         em_tag(__VA_ARGS__)
#define Avatar(...)      em_avatar(__VA_ARGS__)
#define Banner(...)      em_banner(__VA_ARGS__)
#define ProgressBar(...) em_progress(__VA_ARGS__)
#define Button(...)      em_button(__VA_ARGS__)
#define IconButton(...)  em_icon_button(__VA_ARGS__)
#define Toggle(...)      em_toggle(__VA_ARGS__)
#define Checkbox(...)    em_checkbox(__VA_ARGS__)
#define Slider(...)      em_slider(__VA_ARGS__)
#define Stepper(...)     em_stepper(__VA_ARGS__)
#define TextField(...)   em_text_field(__VA_ARGS__)
#define PasswordField(...) em_password_field(__VA_ARGS__)
#define Segmented(...)   em_segmented(__VA_ARGS__)
#define Spacer()         em_spacer_()
/* Divider() as before; Divider("key") when it sits among rows that come and
 * go -- see EmProps.key. The concatenation makes the no-argument form pass an
 * empty key, so every existing call is unchanged. */
#define Divider(...)     em_divider_k_("" __VA_ARGS__)

EmV em_text(const char *s);
EmV em_icon(int codepoint);
EmV em_label(int codepoint, const char *s);
EmV em_badge(const char *s);
EmV em_tag(const char *s);
EmV em_avatar(const char *initials);
EmV em_banner(int codepoint, const char *msg);
EmV em_progress(float frac);
EmV em_button(const char *s);
EmV em_icon_button(int codepoint);
EmV em_toggle(const char *label, bool *bind);
EmV em_checkbox(const char *label, bool *bind);
EmV em_slider(float *bind);
EmV em_stepper(const char *label, int *bind, int lo, int hi);
EmV em_text_field(char *buf, size_t cap, const char *placeholder);
EmV em_password_field(char *buf, size_t cap, const char *placeholder);
EmV em_segmented(const char *const *labels, int count, int *bind);
void em_spacer_(void);
void em_divider_(void);
void em_divider_k_(const char *key);
uint64_t em_key_hash(const char *key);

/* --- richer components --- */
/* Chart: a mini bar chart (values scaled to the max; last bar emphasised). */
#define Chart(vals, n, ...)  em_chart((vals), (n), (EmProps){__VA_ARGS__})
void em_chart(const float *vals, int n, EmProps p);

/* LineChart / AreaChart: a smooth polyline (software-rasterised via a real line
 * primitive) over a bitmap; AreaChart also fills beneath it. One on screen at a
 * time, fixed data. `.height`/`.color` supported. */
#define LineChart(vals, n, ...)  em_linechart((vals), (n), 0, (EmProps){__VA_ARGS__})
#define AreaChart(vals, n, ...)  em_linechart((vals), (n), 1, (EmProps){__VA_ARGS__})
void em_linechart(const float *vals, int n, int filled, EmProps p);

/* List: a grouped, inset surface; ListRow: a tappable row with a trailing
 * chevron. ListRow is chainable (.id / .clicked). */
#define List(...)     EM_SCOPE_(em_list_((EmProps){__VA_ARGS__}), em_list_end_())
#define ListRow(...)  em_listrow(__VA_ARGS__)
void em_list_(EmProps p);
void em_list_end_(void);
EmV  em_listrow(int icon, const char *title, const char *value);

/* ======================================================================= */
/* EmUI V4                                                                  */
/* ======================================================================= */

/* --- App-owned window chrome (the custom close) ------------------------- *
 * A V4 app opens a CHROMELESS OS window (embk_win_create_shared_ex with
 * EMBK_WINF_CHROMELESS) -- no kernel title bar or close button -- and draws
 * its OWN chrome with these. Window() is the full-bleed top-level surface;
 * WindowBar() is a draggable strip whose trailing scope holds the app's own
 * controls (a CloseButton, menus, whatever). Everything is normal toolkit
 * nodes, so the whole bar restyles through EmProps/theme.
 *
 *   Window("Files") {
 *       WindowBar("Files") { if (CloseButton().clicked()) quit(); }
 *       ... app content ...
 *   }
 *
 * Register how the window moves + its id/pos ONCE at startup (keeps the
 * toolkit free of any syscall dependency, exactly like em_set_clock):
 *   em_window_set_mover(my_move);   // my_move calls embk_win_move
 *   em_window_bind(win_id, x, y);   // initial screen position                */
#define Window(title, ...)    EM_SCOPE_(em_window_((title), (EmProps){__VA_ARGS__}), em_window_end_())
#define WindowBar(title, ...) EM_SCOPE_(em_windowbar_((title), (EmProps){__VA_ARGS__}), em_windowbar_end_())
void em_window_(const char *title, EmProps p);
void em_window_end_(void);
void em_windowbar_(const char *title, EmProps p);
void em_windowbar_end_(void);
/* AppBar -- the standard chrome for an OS application window. One definition
 * so Files, Settings and the Terminal are visibly the same product: traffic
 * lights leading (close, then minimize, Mac order), the title centred in the
 * space between, and whatever the app puts in the scope trailing. The whole
 * bar is a drag zone except where a control sits.
 *
 *     Window("Files") {
 *         AppBar("Files") { SearchField(q, sizeof q, "Search"); }
 *         Split(220) { Sidebar() {...} Content() {...} }
 *     }                                                                      */
#define AppBar(title, ...)  EM_SCOPE_(em_appbar_((title), (EmProps){__VA_ARGS__}), em_appbar_end_())
void em_appbar_(const char *title, EmProps p);
void em_appbar_end_(void);

#define CloseButton(...)  em_close_button()
EmV  em_close_button(void);            /* modern single round close control; chainable (.clicked()) */
int  em_window_closed(void);           /* 1 if the built-in CloseButton fired this frame */
#define MinimizeButton(...) em_min_button()
EmV  em_min_button(void);              /* park the window; the dock icon brings it back */
int  em_window_minimized(void);        /* 1 if the built-in MinimizeButton fired this frame */
/* CloseGrip: EmbLink's own close GESTURE (not a fixed button). Put it in the
 * WindowBar; the user PULLS it -- the window fades + slides toward the drag and
 * closes once pulled past the threshold (springs back if released early), the
 * same drag-to-commit shape as the resize corner grip. The EM_APPLICATION
 * runtime handles the actual teardown (via em_window_take_close). */
#define CloseGrip()  em_close_grip()
bool em_close_grip(void);
int  em_window_take_close(void);       /* 1 the frame the close gesture committed */
int  em_window_pulling(void);          /* 1 while a close pull is animating (fade/slide) */
void em_window_set_mover(void (*mover)(int win, int32_t x, int32_t y));
void em_window_bind(int win, int32_t x, int32_t y);
void em_window_move_to(int32_t x, int32_t y);   /* snap/pin the bound window */
void em_window_pos(int32_t *x, int32_t *y);     /* current bound-window top-left */
int  em_window_moved(void);                     /* read-and-clear: window moved since last poll */

/* DragHandle: a placeable strip that drags the bound window (menu-bar move). */
void em_drag_handle_(EmProps p);
void em_drag_handle_end_(void);
#define DragHandle(...) EM_SCOPE_(em_drag_handle_((EmProps){__VA_ARGS__}), em_drag_handle_end_())

/* Resizable windows (V5): the runtime enables the grip; Window() then draws a
 * corner handle whose drag, ON RELEASE, records a size delta the runtime
 * collects with em_window_take_resize and applies via embk_win_resize. */
void em_window_set_resizable(int on);
void em_window_set_glass(int on);   /* Window() renders a translucent tint for glass */
int  em_window_take_resize(int *dw, int *dh);   /* 1 if a resize is pending */

/* --- new components ----------------------------------------------------- */
/* Dropdown / Picker: a field showing labels[*sel]; tap to open a menu, tap an
 * item to pick it. Manages its own open/closed state (one open at a time). */
EmV  em_dropdown(const char *const *labels, int count, int *sel);
#define Dropdown(labels, count, sel)  em_dropdown((labels), (count), (sel))

/* Toast: transient message; call em_toast() to raise one, ToastHost() once at
 * the root (LAST, so it floats on top) to render whatever is active. */
void em_toast(const char *msg, EmTone tone);
void em_toast_host(void);
#define ToastHost()  em_toast_host()

/* Spinner: indeterminate activity (phase-animated dots). Gauge: ring progress
 * 0..1, rasterised (like LineChart). SearchField: text field + search/clear. */
EmV  em_spinner(void);
#define Spinner(...)  em_spinner()
void em_gauge(float frac, const char *center, EmProps p);
#define Gauge(frac, center, ...)  em_gauge((frac), (center), (EmProps){__VA_ARGS__})

/* ColorPicker: an HSV square + rainbow hue bar, both draggable, with a live
 * swatch + hex readout. Binds to float hsv[3] = { hue, saturation, value },
 * all in 0..1. `em_hsv` converts an HSV triple to a Color for use elsewhere. */
Color em_hsv(float h, float s, float v);
void  em_colorpicker(float *hsv, EmProps p);
#define ColorPicker(hsv, ...)  em_colorpicker((hsv), (EmProps){__VA_ARGS__})

/* Calendar / DatePicker: an inline month grid. Binds to int date[3] =
 * { year, month(1..12), day(1..31) }; tap a day to select, ‹ / › page months. */
void em_calendar(int *date, EmProps p);
#define Calendar(date, ...)    em_calendar((date), (EmProps){__VA_ARGS__})
#define DatePicker(date, ...)  em_calendar((date), (EmProps){__VA_ARGS__})

/* Combobox: an editable field whose menu shows only the options containing the
 * typed text (case-insensitive); picking one fills the buffer. `open` is
 * app-owned (like Disclosure); the chevron toggles it. */
void em_combobox(char *buf, size_t cap, const char *const *labels, int count,
                 const char *placeholder, bool *open, EmProps p);
#define Combobox(buf, cap, labels, count, ph, open, ...) \
    em_combobox((buf), (cap), (labels), (count), (ph), (open), (EmProps){__VA_ARGS__})

/* TagInput: removable chips + an entry field. `tags` is char[max][EM_TAG_LEN];
 * *count is the live count; `entry` holds the in-progress tag. */
#define EM_TAG_LEN 24
void em_taginput(char (*tags)[EM_TAG_LEN], int *count, int max,
                 char *entry, size_t ecap, EmProps p);
#define TagInput(tags, count, max, entry, ecap, ...) \
    em_taginput((tags), (count), (max), (entry), (ecap), (EmProps){__VA_ARGS__})
EmV  em_search_field(char *buf, size_t cap, const char *placeholder);
#define SearchField(buf, cap, ph)  em_search_field((buf), (cap), (ph))

/* Disclosure: a tappable header that shows/hides its scope children. */
#define Disclosure(title, open, ...) \
    EM_SCOPE_(em_disclosure_((title), (open), (EmProps){__VA_ARGS__}), em_disclosure_end_())
void em_disclosure_(const char *title, bool *open, EmProps p);
void em_disclosure_end_(void);

/* StatCard: a dashboard tile -- label, big value, signed delta, mini sparkline
 * (vals may be NULL). EmptyState: centred icon + title + subtitle block.
 * DividerLabel: a line--LABEL--line separator. */
void em_stat_card(const char *label, const char *value, const char *delta,
                  const float *vals, int n);
#define StatCard(label, value, delta, vals, n)  em_stat_card((label),(value),(delta),(vals),(n))
void em_empty_state(int icon, const char *title, const char *subtitle);
#define EmptyState(icon, title, subtitle)  em_empty_state((icon),(title),(subtitle))
void em_divider_label(const char *label);
#define DividerLabel(label)  em_divider_label(label)

/* --- navigation: tabs + split view -------------------------------------- */
typedef void (*EmPage)(void);   /* a page/tab is just a view function */
typedef struct { int icon; const char *label; EmPage page; } EmTab;
/* Renders items[*sel].page, then a bottom tab bar (icon+label, eased selection
 * pill). Compose with NavigationStack -- a tab's page may Push/Pop. */
void em_tabview(int *sel, const EmTab *items, int count);
#define TabView(sel, items, count)  em_tabview((sel), (items), (count))

/* SplitView: a fixed-width sidebar surface + a growing content pane. */
#define Split(sidew, ...)   EM_SCOPE_(em_split_((sidew), (EmProps){__VA_ARGS__}), em_split_end_())
#define SidebarPane(...)    EM_SCOPE_(em_sidebar_((EmProps){__VA_ARGS__}), em_end_())
#define ContentPane(...)    EM_SCOPE_(em_content_((EmProps){__VA_ARGS__}), em_end_())
void em_split_(float sidebar_w, EmProps p);
void em_split_end_(void);
void em_sidebar_(EmProps p);
void em_content_(EmProps p);

/* ======================================================================= */
/* EmUI V6 -- menus (menu bar, dropdown menus, context menus)               */
/* ======================================================================= *
 * A proper desktop menu system, built on the modal-overlay layer (so menus
 * float above content, and a click anywhere else closes them). One menu is
 * open at a time.
 *
 *   MenuBar {
 *       Menu("File") {
 *           if (MenuItem("New").shortcut("Ctrl+N").clicked()) new_doc();
 *           MenuSeparator();
 *           if (MenuItem("Quit").clicked()) quit();
 *       }
 *       Menu("Edit") { ... }
 *   }
 *
 *   // right-click anywhere -> a context menu at the pointer:
 *   static bool ctx; static float cx, cy;
 *   if (RightClicked(&cx, &cy)) ctx = true;
 *   ContextMenu(&ctx, cx, cy) { if (MenuItem("Copy").clicked()) copy(); }
 */

/* MenuBar: a horizontal strip of Menu buttons (put it at the top of a Window). */
#define MenuBar(...)  EM_SCOPE_(em_menubar_((EmProps){__VA_ARGS__}), em_menubar_end_())
void em_menubar_(EmProps p);
void em_menubar_end_(void);

/* Menu: a labeled button in the bar; its brace scope holds the MenuItems, shown
 * as a floating popover anchored below the button when open (click to toggle). */
#define Menu(label, ...)  EM_SCOPE_(em_menu_((label), (EmProps){__VA_ARGS__}), em_menu_end_())
void em_menu_(const char *label, EmProps p);
void em_menu_end_(void);
int  em_menu_any_open(void);   /* is any MenuBar dropdown open? (app runtime grows the window) */

/* MenuItem: one row; returns true the frame it's clicked (which also closes the
 * open menu). MenuItemK adds a right-aligned shortcut hint ("Ctrl+S"). */
bool em_menu_item(const char *label, const char *shortcut);
#define MenuItem(label)       em_menu_item((label), 0)
#define MenuItemK(label, sc)  em_menu_item((label), (sc))
void em_menu_separator(void);
#define MenuSeparator()  em_menu_separator()

/* Right-click detection: returns 1 (once) on a right-press this frame, with the
 * press position written to out_x and out_y (window-content coords). Drive from
 * app's pointer state -- em_app_run does this automatically. */
int  em_right_clicked(float *out_x, float *out_y);
#define RightClicked(px, py)  em_right_clicked((px), (py))

/* ContextMenu: a popover anchored at (x,y) while *open; its scope holds
 * MenuItems. Clicking an item or outside closes it (clears *open). */
#define ContextMenu(open, x, y, ...) \
    EM_SCOPE_(em_context_menu_((open), (x), (y), (EmProps){__VA_ARGS__}), em_context_menu_end_())
void em_context_menu_(bool *open, float x, float y, EmProps p);
void em_context_menu_end_(void);

/* Runtime plumbing: the app loop feeds raw right-button state here (em_app_run
 * does it). A press EDGE of the right button becomes one em_right_clicked(). */
void em_feed_right_button(float x, float y, bool down);

/* ======================================================================= */
/* EmUI V7 -- multi-line text editing                                       */
/* ======================================================================= *
 * TextEditor: an editable multi-line text area. `buf`/`cap` own the text (a
 * NUL-terminated C string the app keeps), `cursor` is a byte offset into it the
 * app persists across frames, `height` is the visible height (it scrolls when
 * the text is taller). Handles typing, Enter (newline), Backspace, Delete, Tab,
 * and cursor movement via the arrow / Home / End keys (which the kernel
 * keyboard driver delivers as the EMBK_KEY_* private codes). Returns true while
 * focused. Click to focus.
 *
 *   static char  doc[4096] = "Hello\nEmbLink.";
 *   static int   cur = 0;
 *   TextEditor(doc, sizeof doc, &cur, 240);
 */
bool em_text_editor(char *buf, size_t cap, int *cursor, float height);
#define TextEditor(buf, cap, cursor, height) em_text_editor((buf), (cap), (cursor), (height))

/* Structural-change epoch: bumps when a V4/V6 component restructures the tree
 * (dropdown/menu open/close, toast raise/expiry, disclosure toggle, tab switch).
 * An app's loop compares it across frames and forces a full repaint on change. */
int em_ui_epoch(void);

/* An APP restructuring its OWN view must say so, for exactly the same reason
 * the built-in components do. Adding or removing a row changes where every
 * later row sits, and the runtime's partial repaint only knows about the boxes
 * it was told changed -- so the frame lands with the new layout drawn over the
 * old pixels. A browser inserting a "loading" strip showed this perfectly: the
 * app bar vanished and the previous page appeared 56px out of place.
 *
 * Call this on the EDGE, when the shape changes, not every frame -- a
 * permanent bump means a permanent full repaint. */
void em_structure_changed(void);

/* ======================================================================= */
/* EmApplication -- the declarative app runtime (V4.1).                     */
/* One macro replaces the whole main(): font/resource setup, arenas, theme, */
/* window creation (kernel-chrome or chromeless), the mover binding, the    */
/* event loop with RETAINED updates (idle frames run NO ui work at all),    */
/* dirty-rect presents, and the app's own-close / ESC teardown.             */
/*                                                                          */
/*     EM_APPLICATION {                                                     */
/*         .title  = "V4 Demo",                                             */
/*         .size   = { 700, 560 },                                          */
/*         .theme  = Dark,                                                  */
/*         .chrome = Chromeless,                                            */
/*         .view   = app,                                                   */
/*     };                                                                   */
/* ======================================================================= */

typedef enum { ThemeAuto = 0, Light, Dark } EmTheme;
typedef enum { ChromeKernel = 0, Chromeless } EmChrome;
typedef enum { FixedSize = 0, Resizable } EmResize;
/* Window material. Acrylic = a frosted GLASS window: the compositor blurs the
 * desktop behind it and composites the window's translucent pixels over it, so
 * the title bar and gaps show the wallpaper frosted (implies Chromeless).
 * Translucent = per-pixel transparent, NO blur: the window's empty pixels reveal
 * the SHARP desktop, so a thin bar can carry a tall invisible canvas whose only
 * painted pixels are the bar strip and its open dropdowns (implies Chromeless). */
typedef enum { MaterialSolid = 0, Acrylic, Translucent } EmMaterial;

typedef struct {
    const char *title;
    struct { int w, h; } size;   /* content size; clamped to the screen */
    EmTheme     theme;
    EmChrome    chrome;          /* Chromeless -> the view draws Window/WindowBar */
    EmResize    resize;          /* Resizable -> Window() shows a corner grip;
                                  * dragging it resizes the OS window (V5) */
    EmMaterial  material;        /* Acrylic -> frosted glass window (V8) */
    int         fullscreen;      /* occupy the current display, without a frame */
    void      (*view)(void);     /* the whole UI, rebuilt only when needed */
    const char *font;            /* resource path; default "/system/fonts/font.ttf" */
    int         pace_ms;         /* loop pace while active; default 10 */
    int         refresh_ms;      /* rebuild the view this often even with no
                                  * input -- for a view showing something the
                                  * WORLD changes (a clock, the wallpaper's
                                  * brightness). 0 = input/animation only, the
                                  * default and right answer for most apps.
                                  * Same meaning as EmWidget's field. */
} EmApp;

void  em_set_viewport(float w, float h);   /* the app runtime feeds this */
/* Feed one frame of pointer state to the toolkit (pointer + right button +
 * wheel). Any loop that is not em_app_run MUST call this -- see em.c. */
void  em_feed_pointer(float x, float y, int left_down, int right_down,
                      int wheel, int focused);
void  em_window_blur_rect(int x, int y, int w, int h);  /* frost behind a sub-rect */
float em_viewport_width(void);
float em_viewport_height(void);

/* ---- terminal-shaped runtime hooks (V8) --------------------------------- *
 * For apps whose real input/output is a byte stream (the Terminal hosting
 * the shell), not toolkit widgets:
 *   - key hook: sees every key BEFORE the toolkit (and before the runtime's
 *     ESC-quits default). Return 1 = consumed, 0 = pass through.
 *   - idle hook: runs EVERY loop iteration, even on untouched frames --
 *     poll external state (embk_fd_avail on a pipe) and em_request_frame()
 *     when something arrived. Keep it cheap; it runs at loop pace.
 * Both are target-runtime features (em_app.c); NULL = off (the default). */
void em_set_key_hook(int (*fn)(int ch));
void em_set_idle_hook(void (*fn)(void));
/* Runs after LAYOUT and before the frame is rendered -- the only moment at
 * which every node's resolved position is known and the pixels have not been
 * produced yet. That is exactly what a text SELECTION needs: where the words
 * ended up, in time to mark the selected ones. NULL = off. */
void em_set_post_layout_hook(void (*fn)(void));

/* PAGE ZOOM. A bracket the caller opens around the content it emits, not a
 * global setting -- so a browser can scale the document and leave its own
 * chrome alone, which is the difference between page zoom and UI zoom. 1.0 is
 * off; set it back when the content ends. */
float em_len(float v, float dflt);
void  em_set_text_scale(float s);
float em_text_scale(void);

/* ---- desktop widgets (V5) ----------------------------------------------- *
 * A widget is a small always-on-desktop window: chromeless, z-banded ABOVE
 * the desktop but BELOW every app window. No keyboard, no close button --
 * it lives until its process is killed. `refresh_ms` re-runs the view on a
 * timer (a clock widget sets 1000), on top of the usual retained triggers.
 *
 *     EM_WIDGET { .title="Clock", .size={190,96}, .pos={24,24},
 *                 .refresh_ms=1000, .view=ClockView };                       */
typedef struct {
    const char *title;
    struct { int w, h; } size;
    struct { int x, y; } pos;
    EmTheme     theme;
    EmMaterial  material;        /* Acrylic -> frosted glass widget (V8) */
    void      (*view)(void);
    int         refresh_ms;      /* periodic rebuild; 0 = input/animation only */
    const char *font;
    int         pace_ms;
} EmWidget;

int em_widget_run(const EmWidget *wg);

#define EM_WIDGET \
    static EmWidget em_widget_spec_; \
    int main(void) { return em_widget_run(&em_widget_spec_); } \
    static EmWidget em_widget_spec_ =

/* Runs the app until its CloseButton/ESC quits. Defined in em_app.c, which is
 * only linked on-target (it speaks the EmbLink SDK); host tests never call it. */
int em_app_run(const EmApp *app);
/* Ask the EM_APPLICATION runtime to close cleanly after the current UI pass.
 * Unlike calling exit() from a view callback, this destroys the compositor
 * window first, so the next screen never inherits stale pixels. */
void em_app_request_exit(int code);

/* Tick the view every `ms` regardless of input; -1 restores the app's own
 * refresh_ms. For work the app is WAITING on rather than driving -- a fetch in
 * flight, a job it is polling. em_request_frame() cannot express this: it is
 * set from inside the view, so the first frame that does not ask stops the
 * view running at all, and nothing can ask again. */
void em_app_set_refresh(int ms);

#define EM_APPLICATION     static EmApp em_app_spec_;     int main(void) { return em_app_run(&em_app_spec_); }     static EmApp em_app_spec_ =

/* --- retained updates ---------------------------------------------------- *
 * The runtime SKIPS build+layout+render entirely on frames where nothing
 * changed (no pointer/key/wheel edge, no epoch bump, no requested frame).
 * Anything that animates asks for the next frame while it is live:
 * Spinner/Toast/em_animate/nav transitions do this automatically; an app
 * whose OWN state changes outside input (a clock, async data) calls
 * em_request_frame() itself when the state changes. */
void em_request_frame(void);
int  em_take_frame_request(void);   /* runtime-internal: read-and-clear */

/* --- resources ----------------------------------------------------------- *
 * Path-keyed caches for fonts and images, IO-agnostic: the host installs an
 * fopen-based loader, the target an embk-based one (em_app_run does this
 * automatically). em_font loads+registers a TTF once (and installs the text
 * backend on first success); em_image decodes a P6 .ppm once into BGRA-premul.
 * Image() shows a cached image scaled to .height. em_theme_use switches the
 * palette (Auto currently = Dark). */
void em_res_set_loader(uint8_t *(*load)(const char *path, size_t *out_len));
uint32_t em_font(const char *path);
const uint32_t *em_image(const char *path, uint32_t *out_w, uint32_t *out_h);
/* Icon resolved at the size it will be DRAWN: a .eic container (docs/ICONS.md)
 * carries the icon at several resolutions and this returns the level that fits
 * want_px, so the blit is 1:1 rather than a resample of one oversized master.
 * Non-.eic paths fall through to em_image, so .ppm/.pam art still works. */
const uint32_t *em_image_at(const char *path, uint32_t want_px,
                            uint32_t *out_w, uint32_t *out_h);
void em_image_view(const char *path, EmProps p);
#define Image(path, ...)  em_image_view((path), (EmProps){__VA_ARGS__})
bool em_image_button(const char *path, float size);
#define ImageButton(path, size) em_image_button((path), (size))
/* Icon button drawn from real art but COLOURED BY THE THEME (the image is used
 * as a stencil), so it sits beside glyph controls and follows theme changes. */
bool em_image_button_tinted(const char *path, float size, Color tint);
#define ImageButtonTinted(path, size, tint) em_image_button_tinted((path), (size), (tint))
bool em_image_button_key(const char *path, float size, uint64_t key);
#define ImageButtonKey(path, size, key) \
    em_image_button_key((path), (size), (uint64_t)(uintptr_t)(key))
void em_background_image(const char *path);
#define BackgroundImage(path) em_background_image(path)
void em_theme_use(EmTheme t);

/* frame + interaction plumbing */
void em_new_frame(void);              /* reset pending + clear id map, tick the clock (once per frame) */
void em_flush(void);                  /* emit the pending element now */
bool Clicked(const char *id);         /* did the element with this id fire this frame? */
bool Hovered(const char *id);

/* --- animation ---
 * Plug in a millisecond clock (embk_uptime_ms on target), then animate values.
 * em_animate eases a per-id retained scalar toward `target` at `rate` (~ how
 * fast; 8-14 feels good) and returns the current value -- drive a ProgressBar,
 * a colour, an offset, anything. Frame-rate independent. */
void em_set_clock(uint64_t (*now_ms)(void));
uint64_t em_now_ms(void);   /* current time (0 if no clock set) */
int   em_dt_ms(void);                 /* ms since the previous frame */
float em_animate(const char *id, float target, float rate);
#define Animate(id, target, rate) em_animate((id), (target), (rate))

/* Reusable views are just C functions. This is optional sugar for the no-arg
 * case so `Component(ProfileCard) { ... }` reads nicely; call it as ProfileCard(). */
#define Component(name) void name(void)

/* ------------------------------------------------------------------------- */
/* navigation -- a page stack of view functions                              */
/*     void HomePage(void) { ... if (Button("Settings").clicked()) Push(Settings); }  */
/*     void Settings(void) { NavBar("Settings"){ if(IconButton(IconChevronL).clicked()) Pop(); } ... } */
/*     // at the top level, each frame:  NavigationStack(HomePage);            */
/* ------------------------------------------------------------------------- */

void em_nav(EmPage root);      /* render the current top page (seeds the stack with root) */
void em_push(EmPage page);     /* navigate forward */
void em_pop(void);             /* navigate back */
int  em_nav_depth(void);       /* how many pages are on the stack */
int  em_nav_transitioning(void); /* !=0 while a page fade is in flight (force a full repaint) */

/* ---- modal overlay (Sheet / Alert) ------------------------------------- *
 * Declare an Overlay LAST in the screen so it paints on top. It dims the page
 * behind and centres a Dialog. OverlayDismissed() is true when the bare scrim
 * (outside the dialog) was clicked. em_overlay_active() tells the host loop to
 * force a full repaint while a modal is up (a page/modal show-hide is a big
 * structural change the dirty-rect renderer won't fully erase). */
#define Overlay()    EM_SCOPE_(em_overlay_(), em_overlay_end_())
#define Dialog(...)  EM_SCOPE_(em_dialog_((EmProps){__VA_ARGS__}), em_dialog_end_())
#define OverlayDismissed()  em_overlay_dismissed()
void em_overlay_(void);
void em_overlay_end_(void);
int  em_overlay_dismissed(void);
int  em_overlay_active(void);
void em_dialog_(EmProps p);
void em_dialog_end_(void);
#define NavigationStack(root) em_nav(root)
#define Page(fn)              (fn)     /* a page is just its view function */
#define Push(page)           em_push(page)
#define Pop()                em_pop()

/* ------------------------------------------------------------------------- */
/* icon codepoints (rendered as font glyphs -- DejaVu Sans has these)         */
/* ------------------------------------------------------------------------- */

#define IconCheck     0x2713   /* checkmark */
#define IconClose     0x2715   /* multiplication x */
#define IconStar      0x2605   /* filled star */
#define IconHeart     0x2665   /* heart */
#define IconChevronR  0x203A   /* single right angle quote */
#define IconChevronL  0x2039
#define IconPlus      0x002B
#define IconMinus     0x2212   /* minus sign */
#define IconGear      0x2699   /* gear */
#define IconBell      0x1F514  /* bell (may fall back) */
#define IconInfo      0x2139   /* information source */
#define IconWarn      0x26A0   /* warning sign */
#define IconBolt      0x26A1   /* high voltage */
#define IconSearch    0x1F50D
#define IconUser      0x1F464
#define IconDot       0x2022   /* bullet */
#define IconArrowR    0x2192
#define IconChevronD  0x2304   /* down arrowhead (Dropdown/Disclosure) */
#define IconChevronU  0x2303
#define IconInbox     0x2617   /* (DejaVu-safe placeholder tray) */
#define IconMagnify   0x26B2   /* magnifier-ish (DejaVu-safe search glyph) */
#define IconFiles     0x25A4   /* lined square (DejaVu-safe files/list) */
#define IconGrid      0x25A6   /* squared grid (dashboard tab) */
#define IconList      0x2630   /* trigram / list (list tab) */
#define IconHome      0x2302   /* house */
#define IconClock     0x1F550
#define IconFolder    0x1F4C1
#define IconDoc       0x1F4C4
#define IconTrash     0x1F5D1
#define IconCloud     0x2601

#endif /* __EMBLINK_EM_UI_H__ */
