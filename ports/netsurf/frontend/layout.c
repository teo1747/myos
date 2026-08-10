/* ports/netsurf/frontend/layout.c -- how wide is this text?
 *
 * The one question the core cannot answer for itself, and the one it asks most:
 * every line break in the document is a width query, and every caret position
 * is a "which character is at x". Get this wrong and the layout is wrong
 * everywhere at once, in a way that looks like a layout bug and is not.
 *
 * Three callbacks, and the second two must AGREE with the first or text jumps
 * when you click it:
 *   width     -- the advance of a whole string
 *   position  -- the character boundary at or before an x offset
 *   split     -- the last space at or before an x offset (where a line breaks)
 *
 * The metrics come from the OS's own font code. Until that is wired through,
 * this uses the same average-advance model the font actually has for its
 * default face, which is honest about being an approximation and is right to
 * within a fraction of a character for Latin text -- and it is stated here
 * rather than discovered by someone measuring a misaligned caret.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"
#include "utils/utf8.h"
#include "emblink.h"


/* plot_font_style carries the size in PLOT_STYLE_SCALE'd points. */
static int px_size(const struct plot_font_style *fstyle)
{
    int px = (fstyle->size * 4) / (3 * PLOT_STYLE_SCALE);   /* points -> px at 96dpi */
    return px > 0 ? px : 1;
}

/* The advance of one UTF-8 string. Counts CHARACTERS, not bytes: a two-byte
 * e-acute is one glyph, and charging it two advances is how a page full of
 * accents ends up wrapping early. */
int emblink_text_advance(const struct plot_font_style *fstyle,
                         const char *utf8, size_t len)
{
    int px = px_size(fstyle);
    size_t chars = 0;
    for (size_t i = 0; i < len; i++)
        if ((utf8[i] & 0xC0) != 0x80) chars++;      /* skip continuations */

    /* 0.5 em for a proportional face, 0.6 for the monospace one -- the
     * measured average advance of the OS's own default faces. */
    int num = (fstyle->family == PLOT_FONT_FAMILY_MONOSPACE) ? 6 : 5;
    return (int)((chars * (size_t)px * (size_t)num) / 10);
}

static nserror layout_width(const struct plot_font_style *fstyle,
                            const char *string, size_t length, int *width)
{
    *width = emblink_text_advance(fstyle, string, length);
    return NSERROR_OK;
}

/* The character boundary at or before `x`. Must land on a UTF-8 boundary, or
 * the core will split a string mid-sequence and hand the renderer half a
 * character. */
static nserror layout_position(const struct plot_font_style *fstyle,
                               const char *string, size_t length,
                               int x, size_t *char_offset, int *actual_x)
{
    size_t best = 0;
    int best_x = 0;

    for (size_t i = 0; i <= length; i++) {
        if (i < length && (string[i] & 0xC0) == 0x80) continue;   /* not a boundary */
        int w = emblink_text_advance(fstyle, string, i);
        if (w > x) break;
        best = i;
        best_x = w;
    }
    *char_offset = best;
    *actual_x = best_x;
    return NSERROR_OK;
}

/* Where a line BREAKS: the last space at or before x. If the first word
 * already overflows there is no break to find, and the core wants the whole
 * word back rather than a split through the middle of it -- an overflowing
 * word hangs out of its box in every browser, it does not get cut in half. */
static nserror layout_split(const struct plot_font_style *fstyle,
                            const char *string, size_t length,
                            int x, size_t *char_offset, int *actual_x)
{
    size_t last_space = 0;
    int last_space_x = 0;
    bool found = false;

    for (size_t i = 0; i < length; i++) {
        if (string[i] != ' ') continue;
        int w = emblink_text_advance(fstyle, string, i);
        if (w > x && found) break;
        last_space = i;
        last_space_x = w;
        found = true;
        if (w > x) break;
    }

    if (!found) {
        *char_offset = length;
        *actual_x = emblink_text_advance(fstyle, string, length);
        return NSERROR_OK;
    }
    *char_offset = last_space;
    *actual_x = last_space_x;
    return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
    .width = layout_width,
    .position = layout_position,
    .split = layout_split,
};

struct gui_layout_table *emblink_layout_table = &layout_table;
