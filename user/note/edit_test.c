/* user/note/edit_test.c -- the editing engine, which is nothing but edge cases.
 *
 * Every one of these was a bug in some editor once: a selection dragged
 * backwards, an undo that restores the text but not the caret, a replace-all
 * whose replacement contains the search term, backspace in indentation.
 * They are two lines each here and a boot-and-squint otherwise. */
#include "edit.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
static void ok(int c, const char *what) {
    printf("  %s: %s\n", c ? "ok  " : "FAIL", what);
    if (!c) g_fail++;
}
static struct editor E;
static char BUF[8192];
static void start(const char *s) { BUF[0] = 0; ed_init(&E, BUF, sizeof BUF); ed_set_text(&E, s); }
static int textis(const char *s) { return strcmp(E.buf, s) == 0; }

int main(void) {
    printf("edit_test\n");

    /* --- typing and lines --- */
    start(""); ed_insert_char(&E, 'a'); ed_insert_char(&E, 'b'); ed_newline(&E); ed_insert_char(&E, 'c');
    ok(textis("ab\nc"), "typing and Enter build lines");
    ok(ed_line_count(&E) == 2, "two lines");
    ok(ed_line_of(&E, E.cursor) == 1 && ed_col_of(&E, E.cursor) == 1, "cursor is at line 1 col 1");

    /* --- auto-indent, the thing that makes Enter usable in code --- */
    start("    foo"); E.cursor = E.anchor = 7; ed_newline(&E);
    ok(textis("    foo\n    "), "Enter carries the line's indentation down");
    start("if (x) {"); E.cursor = E.anchor = 8; ed_newline(&E);
    ok(textis("if (x) {\n    "), "...and indents one level further after {");

    /* --- backspace in indentation removes a LEVEL, not a space --- */
    start("        x"); E.cursor = E.anchor = 8; ed_backspace(&E);
    ok(textis("    x"), "backspace in leading whitespace removes one indent level");
    start("ab"); E.cursor = E.anchor = 2; ed_backspace(&E);
    ok(textis("a"), "...and one character everywhere else");

    /* --- selection, in both directions --- */
    start("hello world");
    E.anchor = 6; E.cursor = 11;
    { int lo, hi; ed_sel_range(&E, &lo, &hi); ok(lo == 6 && hi == 11, "forward selection"); }
    E.anchor = 11; E.cursor = 6;
    { int lo, hi; ed_sel_range(&E, &lo, &hi); ok(lo == 6 && hi == 11, "BACKWARD selection normalises"); }
    { char out[32]; int n = ed_copy(&E, out, sizeof out);
      ok(n == 5 && !strcmp(out, "world"), "copy takes the range, not the direction"); }
    ed_insert_text(&E, "there", 5);
    ok(textis("hello there"), "typing over a selection replaces it");

    /* --- an unshifted arrow collapses a selection to its edge --- */
    start("abcdef"); E.anchor = 1; E.cursor = 4;
    ed_move(&E, +1, 0);
    ok(E.cursor == 4 && !ed_has_sel(&E), "right collapses to the far edge");
    E.anchor = 1; E.cursor = 4;
    ed_move(&E, -1, 0);
    ok(E.cursor == 1, "left collapses to the near edge");

    /* --- goal column: through a short line and back out --- */
    start("longer line\nx\nlonger line");
    E.cursor = E.anchor = 8;                  /* col 8 of line 0 */
    ed_move_line_v(&E, +1, 0);
    ok(ed_col_of(&E, E.cursor) == 1, "down onto a short line clamps to its end");
    ed_move_line_v(&E, +1, 0);
    ok(ed_col_of(&E, E.cursor) == 8, "...and down again REMEMBERS column 8");

    /* --- smart home --- */
    start("    indented"); E.cursor = E.anchor = 12;
    ed_home(&E, 0); ok(ed_col_of(&E, E.cursor) == 4, "Home goes to the first non-blank");
    ed_home(&E, 0); ok(ed_col_of(&E, E.cursor) == 0, "Home again goes to column zero");

    /* --- undo, including the caret --- */
    start("abc"); E.cursor = E.anchor = 3;
    ed_insert_char(&E, 'd'); ed_insert_char(&E, 'e');
    ok(textis("abcde"), "typed two characters");
    ok(ed_undo(&E) && textis("abc"), "one undo removes BOTH -- typing coalesces");
    ok(E.cursor == 3, "...and restores the caret");
    start("hello world"); E.anchor = 0; E.cursor = 5; ed_delete_sel(&E);
    ok(textis(" world"), "deleted a selection");
    ok(ed_undo(&E) && textis("hello world"), "undo brings the deleted text back");
    ok(E.anchor == 0 && E.cursor == 5, "...and the selection with it");
    ok(ed_redo(&E) && textis(" world"), "redo removes it again");

    /* --- indent / outdent a block --- */
    start("a\nb\nc"); E.anchor = 0; E.cursor = 5;
    ed_indent(&E);
    ok(textis("    a\n    b\n    c"), "Tab indents every line the selection touches");
    ed_outdent(&E);
    ok(textis("a\nb\nc"), "Shift-Tab takes it back off");
    start("x"); E.cursor = E.anchor = 0; ed_indent(&E);
    ok(textis("    x"), "Tab with no selection inserts one level");

    /* --- line operations --- */
    start("one\ntwo\nthree"); E.cursor = E.anchor = 5;   /* on "two" */
    ed_duplicate_line(&E);
    ok(textis("one\ntwo\ntwo\nthree"), "duplicate line");
    start("one\ntwo\nthree"); E.cursor = E.anchor = 5;
    ed_delete_line(&E);
    ok(textis("one\nthree"), "delete line");
    start("one\ntwo\nthree"); E.cursor = E.anchor = 5;
    ed_move_line(&E, -1);
    ok(textis("two\none\nthree"), "move line up");
    start("one\ntwo\nthree"); E.cursor = E.anchor = 0;
    ed_move_line(&E, +1);
    ok(textis("two\none\nthree"), "move line down");

    /* --- find --- */
    start("foo bar foo baz");
    ok(ed_find(&E, "foo", 0, 0, 0) == 0, "find from the start");
    ok(ed_find(&E, "foo", 1, 0, 0) == 8, "find the next one");
    ok(ed_find(&E, "foo", 9, 0, 0) == -1, "no match past the last");
    ok(ed_find(&E, "foo", 9, 0, 1) == 0, "...unless it wraps");
    ok(ed_find(&E, "FOO", 0, 1, 0) == 0, "case-insensitive");
    ok(ed_find(&E, "FOO", 0, 0, 0) == -1, "...and case-sensitive when asked");
    ok(ed_find_prev(&E, "foo", 15, 0) == 8, "find backwards");

    /* --- replace-all, including the case that eats itself --- */
    start("a a a");
    ok(ed_replace_all(&E, "a", "aa", 0) == 3 && textis("aa aa aa"),
       "replacing a with aa terminates and does not feed on its output");
    start("xyx");
    ok(ed_replace_all(&E, "x", "", 0) == 2 && textis("y"), "replace with nothing deletes");
    start("no match here");
    ok(ed_replace_all(&E, "zzz", "q", 0) == 0 && textis("no match here"), "nothing matched, nothing changed");

    /* --- brackets --- */
    start("f(a, g(b))");
    ok(ed_match_bracket(&E, 1) == 9, "matching ) across a nested pair");
    ok(ed_match_bracket(&E, 9) == 1, "...and back the other way");
    ok(ed_match_bracket(&E, 0) == -1, "a letter matches nothing");

    /* --- refusing to overflow rather than truncating --- */
    { static char small[16]; small[0] = 0;
      struct editor s; ed_init(&s, small, sizeof small);
      ed_set_text(&s, "0123456789");
      ed_insert_text(&s, "abcdefghij", 10);
      ok(strlen(s.buf) == 10, "an insert that would not fit is REFUSED, not truncated"); }

    printf("=== edit_test: %s (%d failure%s) ===\n",
           g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail != 0;
}
