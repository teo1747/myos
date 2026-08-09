/* user/note/edit.h -- the editing engine behind Note++.
 *
 * Everything that happens to text lives here, and nothing that happens to
 * pixels does. That split is not tidiness: an editor is mostly EDGE CASES --
 * a selection dragged backwards, an undo that has to restore the cursor as
 * well as the characters, a replace-all whose replacement contains the search
 * term -- and every one of them is a two-line host test here and a boot-and-
 * squint on a screen otherwise.
 *
 * The buffer is a flat NUL-terminated array. Not a gap buffer and not a piece
 * table: at the size a person edits by hand, a memmove of a few kilobytes is
 * far below a frame, and the simpler representation means the whole file is
 * always one contiguous string -- which is what the highlighter, the search
 * and the save path all want anyway. When that stops being true the interface
 * here is what stays.
 *
 * A SELECTION is an anchor and a cursor, in that order, and either may be the
 * larger. Keeping them unordered is what makes shift-arrow work in both
 * directions without a special case; every operation that needs a range asks
 * for it normalised.
 */
#ifndef _EMBLINK_NOTE_EDIT_H_
#define _EMBLINK_NOTE_EDIT_H_

#include <stddef.h>

/* Undo depth. Each entry owns a copy of the text it replaced, so this is the
 * memory the feature costs: bounded, like every other arena here. */
#define ED_UNDO_MAX   64
#define ED_UNDO_TEXT  (64 * 1024)

enum ed_op { ED_OP_NONE = 0, ED_OP_INSERT, ED_OP_DELETE, ED_OP_REPLACE };

struct ed_undo {
    int    at;                 /* offset the change started at            */
    int    removed_len;        /* bytes removed there (stored in `text`)  */
    int    added_len;          /* bytes added there                       */
    int    cursor_before;
    int    sel_before;
    int    text_off;           /* into the undo text pool: the REMOVED bytes */
    unsigned char op;
    unsigned char coalesce;    /* may merge with the next typed character */
};

struct editor {
    char  *buf;                /* the text; caller-owned, NUL-terminated  */
    size_t cap;
    int    len;
    int    cursor;             /* byte offset                             */
    int    anchor;             /* selection's other end; == cursor = none */
    int    tab_width;          /* spaces per indent level (default 4)     */
    int    use_tabs;           /* 0 = insert spaces (default)             */
    int    goal_col;           /* remembered column for up/down; -1 = none */

    struct ed_undo undo[ED_UNDO_MAX];
    char   undo_text[ED_UNDO_TEXT];
    int    undo_text_n;
    int    undo_n;             /* entries in use                          */
    int    undo_at;            /* next slot to write == redo boundary     */
};

/* --- lifecycle --- */
void ed_init(struct editor *e, char *buf, size_t cap);
void ed_set_text(struct editor *e, const char *s);   /* replaces all; clears undo */

/* --- selection --- */
int  ed_has_sel(const struct editor *e);
void ed_sel_range(const struct editor *e, int *lo, int *hi);   /* normalised */
void ed_select_all(struct editor *e);
void ed_clear_sel(struct editor *e);
/* Copy the selection out (NUL-terminated). Returns bytes written, 0 if none. */
int  ed_copy(const struct editor *e, char *out, size_t cap);

/* --- editing. Every one of these is undoable. --- */
void ed_insert_text(struct editor *e, const char *s, int n);
void ed_insert_char(struct editor *e, char c);
void ed_newline(struct editor *e);          /* with auto-indent */
void ed_backspace(struct editor *e);
void ed_delete(struct editor *e);
void ed_delete_sel(struct editor *e);
void ed_indent(struct editor *e);           /* Tab: indent line(s) or insert */
void ed_outdent(struct editor *e);          /* Shift-Tab */
void ed_duplicate_line(struct editor *e);
void ed_delete_line(struct editor *e);
void ed_move_line(struct editor *e, int dir);   /* -1 up, +1 down */

/* --- undo/redo --- */
int  ed_undo(struct editor *e);             /* 1 if something was undone */
int  ed_redo(struct editor *e);
int  ed_can_undo(const struct editor *e);
int  ed_can_redo(const struct editor *e);

/* --- movement. `extend` keeps the anchor, i.e. shift-held. --- */
void ed_move(struct editor *e, int delta, int extend);      /* by character */
void ed_move_word(struct editor *e, int dir, int extend);
void ed_move_line_v(struct editor *e, int dir, int extend); /* up/down      */
void ed_home(struct editor *e, int extend);                 /* smart home   */
void ed_end(struct editor *e, int extend);
void ed_doc_start(struct editor *e, int extend);
void ed_doc_end(struct editor *e, int extend);
void ed_page(struct editor *e, int dir, int lines, int extend);
void ed_goto_line(struct editor *e, int line);              /* 1-based      */

/* --- queries --- */
int  ed_line_of(const struct editor *e, int off);           /* 0-based      */
int  ed_col_of(const struct editor *e, int off);            /* 0-based      */
int  ed_line_start(const struct editor *e, int off);
int  ed_line_end(const struct editor *e, int off);
int  ed_line_count(const struct editor *e);
int  ed_offset_of_line(const struct editor *e, int line);

/* --- find / replace. `from` is a byte offset; returns the match offset or -1.
 * Case-insensitive when `icase`. Wraps once when `wrap`. --- */
int  ed_find(const struct editor *e, const char *needle, int from, int icase, int wrap);
int  ed_find_prev(const struct editor *e, const char *needle, int from, int icase);
/* Replace the CURRENT selection if it matches, then find the next. Returns 1
 * if a replacement happened. */
int  ed_replace(struct editor *e, const char *needle, const char *with, int icase);
int  ed_replace_all(struct editor *e, const char *needle, const char *with, int icase);

/* The offset of the bracket matching the one at `off`, or -1. Handles nesting
 * and skips brackets inside strings and comments badly enough to say so: it
 * does not skip them at all, which is honest and occasionally wrong. */
int  ed_match_bracket(const struct editor *e, int off);

/* How many times `needle` occurs. For a find bar that says "3 of 12" -- a
 * search that cannot count is one you have to drive blind. */
int  ed_count(const struct editor *e, const char *needle, int icase);
/* Which occurrence the caret is sitting on, 1-based, or 0 if none. */
int  ed_match_index(const struct editor *e, const char *needle, int icase);

/* The word around `off`, for a double-click. Returns 0 when there is no word
 * there, in which case lo/hi are untouched. */
int  ed_word_at(const struct editor *e, int off, int *lo, int *hi);
void ed_select_word(struct editor *e, int off);
void ed_select_line(struct editor *e, int off);

/* Comment or uncomment every line the selection touches, with `tok` (e.g. "//"
 * or "#"). Uncomments when EVERY touched line already starts with the token --
 * the rule that makes one shortcut do both without a mode. */
int  ed_toggle_comment(struct editor *e, const char *tok);

/* Auto-close: the closing partner for an opening bracket or quote, or 0. */
char ed_auto_close(char open);

#endif /* _EMBLINK_NOTE_EDIT_H_ */
