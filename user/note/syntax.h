/* user/note/syntax.h -- what colour a piece of source is.
 *
 * One line at a time, which is the unit the editor draws in and therefore the
 * only unit worth tokenising. That choice has a consequence stated here rather
 * than discovered later: a construct that SPANS lines -- a block comment, a
 * string with a continuation -- cannot be recognised from the line alone. The
 * caller carries a small state value from one line to the next, so the
 * highlighter stays a pure function of (line, incoming state) and the editor
 * stays free to draw any line at any time without re-scanning the file above
 * it. A highlighter that needed the whole buffer would make scrolling a long
 * file quadratic.
 *
 * Colours are named by ROLE, not by value. The palette lives with the app's
 * theme, so a keyword is "a keyword" here and blue somewhere else.
 */
#ifndef _EMBLINK_NOTE_SYNTAX_H_
#define _EMBLINK_NOTE_SYNTAX_H_

#include <stddef.h>

/* What a run of characters IS. The editor maps these to colours. */
enum syn_role {
    SYN_TEXT = 0,      /* ordinary code                       */
    SYN_KEYWORD,       /* if, while, return                   */
    SYN_TYPE,          /* int, char, struct, our own *_t names */
    SYN_STRING,        /* "..." and '...'                     */
    SYN_COMMENT,       /* line comments and block comments     */
    SYN_NUMBER,        /* 42, 0x2A, 3.14f                     */
    SYN_PREPROC,       /* #include, #define                   */
    SYN_PUNCT,         /* braces, operators                   */
    SYN_ROLE_COUNT
};

/* Which language a file is, decided by its name. */
enum syn_lang {
    SYN_PLAIN = 0,     /* no highlighting at all              */
    SYN_C,             /* C and C++                           */
    SYN_PY,
    SYN_JS,
    SYN_SH,
    SYN_MD,
    SYN_LANG_COUNT
};

/* Pick a language from a path. Unknown extensions are SYN_PLAIN, which is not a
 * failure -- a text file highlighted as C looks worse than one not highlighted
 * at all. */
enum syn_lang syn_lang_of(const char *path);
const char   *syn_lang_name(enum syn_lang l);

/* Carried between lines. 0 is "nothing open"; the highlighter returns the state
 * the NEXT line starts in. Deliberately an int rather than a struct: the editor
 * keeps one per line and a file has a lot of lines. */
enum { SYN_ST_NONE = 0, SYN_ST_BLOCK_COMMENT = 1, SYN_ST_STRING = 2 };

struct syn_span { int len; unsigned char role; };

/* Tokenise `n` bytes of one line. Writes at most `max` spans covering the line
 * EXACTLY -- the lengths sum to n -- and returns how many. `state` is read for
 * the incoming state and written with the outgoing one. */
int syn_line(enum syn_lang lang, const char *line, int n,
             struct syn_span *out, int max, int *state);

#endif /* _EMBLINK_NOTE_SYNTAX_H_ */
