#ifndef __EMBLINK_UI_KIT_H__
#define __EMBLINK_UI_KIT_H__

/* ui/kit/kit.h -- EmbLink UI: the themed widget kit.
 *
 * A thin, opinionated layer over the raw declarative API (ui/declare) that
 * reads exclusively from the design tokens (ui/theme). Its whole job is to make
 * the clean, coherent look the DEFAULT: an app author composes cards, labels,
 * buttons and toggles and gets consistent spacing, colour, radius and
 * elevation without touching a single raw value. */

#include "ui.h"
#include "theme.h"

/* --- structure --- */
void ui_screen_begin(void);   /* full-window page: bg fill, page padding, column */
void ui_screen_end(void);

void ui_card_begin(uint64_t key);   /* elevated surface: border + soft shadow + padding */
void ui_card_end(void);

void ui_panel_begin(uint64_t key);  /* quieter inset surface (surface_alt, hairline) */
void ui_panel_end(void);

void ui_row_begin(uint64_t key);    /* hstack, center-aligned, standard gap */
void ui_row_end(void);
void ui_col_begin(uint64_t key);    /* vstack, standard gap */
void ui_col_end(void);

void ui_divider(void);              /* full-width hairline rule */
void ui_flex_spacer(void);          /* pushes siblings apart on the main axis */
void ui_gap(float px);              /* fixed-size spacer */

/* --- typography (role == size + weight + colour from the tokens) --- */
void ui_heading(const char *fmt, ...);
void ui_title(const char *fmt, ...);
void ui_body(const char *fmt, ...);
void ui_secondary(const char *fmt, ...);
void ui_caption(const char *fmt, ...);

/* --- controls (return true on the frame they were clicked) --- */
bool ui_button_primary(const char *label);
bool ui_button_secondary(const char *label);
bool ui_button_ghost(const char *label);
bool ui_toggle(bool on);            /* returns true if clicked -> caller flips its state */

/* checkbox / radio: return true on the frame clicked -> caller flips its state */
bool ui_checkbox(bool on);
bool ui_radio(bool selected);

/* progress: a filled track, frac in [0,1], display-only */
void ui_progress(float frac);

/* slider: drag anywhere on the track; returns the (possibly updated) value [0,1] */
float ui_slider(float value);

/* segmented control (tabs): a row of `count` labels; returns the selected index,
 * updated to whatever was clicked this frame. */
int ui_segmented(const char *const *labels, int count, int selected);

/* chip: a compact, clickable pill (like a filter tag). true on the click frame. */
bool ui_chip(const char *label, bool active);

/* avatar: a tinted circle with 1-2 initials */
void ui_avatar(const char *initials);

/* text field: an editable single-line input. `buf` (cap bytes, NUL-terminated)
 * is the app-owned edit buffer; click to focus, then typed keys (fed via
 * ui_input_char in the loop) edit it in place. Draws a caret when focused and a
 * placeholder when empty+unfocused. Returns true while focused. */
/* A CONTROL PALETTE, for controls that are not part of the desktop.
 *
 * The kit's controls take their colours from the theme, which is right for an
 * application and wrong inside a rendered document: a web page is drawn on its
 * own white canvas, so a text field on it came out as a dark rounded box with
 * the desktop's accent -- the browser's chrome leaking into the page, which is
 * the same mistake as drawing the page's text in the desktop's ink.
 *
 * Set it around content that owns its own look and clear it afterwards, the
 * way a browser brackets the document it emits. NULL restores the theme. An
 * unset entry (alpha 0) falls back to the theme, so a caller can override one
 * colour without restating them all. */
struct ui_ctl_palette {
    struct color surface, border, focus, text, placeholder;
};
void ui_set_control_palette(const struct ui_ctl_palette *p);

/* One colour from it, or `dflt` when the palette is unset or leaves that entry
 * transparent. For controls built OUTSIDE the kit (the DSL's Dropdown) that
 * still have to follow it. */
enum { UI_CTL_SURFACE, UI_CTL_BORDER, UI_CTL_FOCUS, UI_CTL_TEXT, UI_CTL_PLACEHOLDER };
struct color ui_ctl_color_(int which, struct color dflt);

bool ui_text_field(char *buf, unsigned long cap, const char *placeholder);
bool ui_password_field(char *buf, unsigned long cap, const char *placeholder);

/* --- scroll view --- */
/* A fixed-height viewport that clips + vertically scrolls its children. `scroll_y`
 * is the app-owned scroll position (px from top); the wheel over the view and a
 * drag inside it both update it, clamped to the content. Put content between. */
void ui_scroll_begin(uint64_t key, float viewport_h, float *scroll_y);
void ui_scroll_end(void);

/* --- overlay / modal --- */
/* A full-surface scrim that dims everything and centres its content. Declare it
 * LAST in the screen so it paints on top. ui_overlay_end() returns true when the
 * scrim (not the dialog) was clicked -- the app treats that as "dismiss". */
void ui_overlay_begin(uint64_t key);
bool ui_overlay_end(void);
void ui_dialog_begin(uint64_t key);   /* an elevated card centred in the overlay */
void ui_dialog_end(void);

/* --- accents --- */
enum ui_badge_tone { BADGE_ACCENT, BADGE_SUCCESS, BADGE_WARNING, BADGE_DANGER, BADGE_NEUTRAL };
void ui_badge(const char *label, enum ui_badge_tone tone);

#endif /* __EMBLINK_UI_KIT_H__ */
